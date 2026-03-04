#include "modbus_rtu.h"

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "modbus_rtu";

#ifndef ESP_ERR_INVALID_CRC
#define ESP_ERR_INVALID_CRC ESP_ERR_INVALID_RESPONSE
#endif

static uint16_t modbus_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if ((crc & 0x0001U) != 0U) {
                crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static void rtu_set_tx(modbus_rtu_ctx_t *ctx) {
    if (ctx->cfg.de_pin >= 0) {
        gpio_set_level(ctx->cfg.de_pin, 1);
    }
    if (ctx->cfg.re_pin >= 0) {
        gpio_set_level(ctx->cfg.re_pin, 1);
    }
}

static void rtu_set_rx(modbus_rtu_ctx_t *ctx) {
    if (ctx->cfg.de_pin >= 0) {
        gpio_set_level(ctx->cfg.de_pin, 0);
    }
    if (ctx->cfg.re_pin >= 0) {
        gpio_set_level(ctx->cfg.re_pin, 0);
    }
}

static esp_err_t rtu_transceive(
    modbus_rtu_ctx_t *ctx,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len_expected,
    size_t *rx_len_actual) {
    if (!ctx->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    *rx_len_actual = 0;

    uart_flush_input(ctx->cfg.uart_num);
    rtu_set_tx(ctx);
    int written = uart_write_bytes(ctx->cfg.uart_num, (const char *)tx, tx_len);
    if (written != (int)tx_len) {
        rtu_set_rx(ctx);
        return ESP_FAIL;
    }
    esp_err_t err = uart_wait_tx_done(ctx->cfg.uart_num, pdMS_TO_TICKS(ctx->cfg.timeout_ms));
    rtu_set_rx(ctx);
    if (err != ESP_OK) {
        return err;
    }

    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ctx->cfg.timeout_ms);
    while (total < (int)rx_len_expected) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            break;
        }
        TickType_t remain = deadline - now;
        int r = uart_read_bytes(ctx->cfg.uart_num, rx + total, rx_len_expected - (size_t)total, remain);
        if (r > 0) {
            total += r;
        } else {
            break;
        }
    }
    *rx_len_actual = (size_t)total;
    return (*rx_len_actual >= 5U) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t modbus_rtu_init(modbus_rtu_ctx_t *ctx, const modbus_rtu_config_t *cfg) {
    if (ctx == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;

    uart_word_length_t data_bits = UART_DATA_8_BITS;
    if (cfg->data_bits == 7U) {
        data_bits = UART_DATA_7_BITS;
    }
    uart_stop_bits_t stop_bits = cfg->stop_bits == 2U ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    uart_parity_t parity = UART_PARITY_DISABLE;
    if (cfg->parity == 'E') {
        parity = UART_PARITY_EVEN;
    } else if (cfg->parity == 'O') {
        parity = UART_PARITY_ODD;
    }

    uart_config_t uart_cfg = {
        .baud_rate = (int)cfg->baud_rate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(cfg->uart_num, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(cfg->uart_num, &uart_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    if (cfg->de_pin >= 0) {
        gpio_reset_pin(cfg->de_pin);
        gpio_set_direction(cfg->de_pin, GPIO_MODE_OUTPUT);
    }
    if (cfg->re_pin >= 0) {
        gpio_reset_pin(cfg->re_pin);
        gpio_set_direction(cfg->re_pin, GPIO_MODE_OUTPUT);
    }
    rtu_set_rx(ctx);

    ctx->initialized = true;
    ESP_LOGI(
        TAG,
        "rtu init uart=%d tx=%d rx=%d de=%d re=%d baud=%" PRIu32 " slave=%u",
        cfg->uart_num,
        cfg->tx_pin,
        cfg->rx_pin,
        cfg->de_pin,
        cfg->re_pin,
        cfg->baud_rate,
        cfg->slave_id);
    return ESP_OK;
}

esp_err_t modbus_rtu_read_registers(
    modbus_rtu_ctx_t *ctx,
    d3net_reg_type_t reg_type,
    uint16_t address,
    uint16_t count,
    uint16_t *out_words) {
    if (ctx == NULL || out_words == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t req[8];
    req[0] = ctx->cfg.slave_id;
    req[1] = (reg_type == D3NET_REG_HOLDING) ? 0x03 : 0x04;
    req[2] = (uint8_t)(address >> 8U);
    req[3] = (uint8_t)(address & 0xFFU);
    req[4] = (uint8_t)(count >> 8U);
    req[5] = (uint8_t)(count & 0xFFU);
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFFU);
    req[7] = (uint8_t)(crc >> 8U);

    uint8_t resp[260];
    size_t exp = (size_t)(5U + count * 2U);
    if (exp > sizeof(resp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t got = 0;
    esp_err_t err = rtu_transceive(ctx, req, sizeof(req), resp, exp, &got);
    if (err != ESP_OK) {
        return err;
    }
    if (got < exp) {
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != ctx->cfg.slave_id || resp[1] != req[1] || resp[2] != (uint8_t)(count * 2U)) {
        return ESP_FAIL;
    }
    uint16_t rx_crc = (uint16_t)resp[got - 2] | ((uint16_t)resp[got - 1] << 8U);
    uint16_t calc_crc = modbus_crc16(resp, got - 2);
    if (rx_crc != calc_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    for (uint16_t i = 0; i < count; i++) {
        out_words[i] = ((uint16_t)resp[3 + i * 2U] << 8U) | resp[4 + i * 2U];
    }
    return ESP_OK;
}

esp_err_t modbus_rtu_write_registers(
    modbus_rtu_ctx_t *ctx,
    uint16_t address,
    uint16_t count,
    const uint16_t *words) {
    if (ctx == NULL || words == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t req[256];
    size_t req_len = (size_t)(9U + count * 2U);
    if (req_len > sizeof(req)) {
        return ESP_ERR_INVALID_SIZE;
    }
    req[0] = ctx->cfg.slave_id;
    req[1] = 0x10;
    req[2] = (uint8_t)(address >> 8U);
    req[3] = (uint8_t)(address & 0xFFU);
    req[4] = (uint8_t)(count >> 8U);
    req[5] = (uint8_t)(count & 0xFFU);
    req[6] = (uint8_t)(count * 2U);
    for (uint16_t i = 0; i < count; i++) {
        req[7 + i * 2U] = (uint8_t)(words[i] >> 8U);
        req[8 + i * 2U] = (uint8_t)(words[i] & 0xFFU);
    }
    uint16_t crc = modbus_crc16(req, req_len - 2U);
    req[req_len - 2U] = (uint8_t)(crc & 0xFFU);
    req[req_len - 1U] = (uint8_t)(crc >> 8U);

    uint8_t resp[8];
    size_t got = 0;
    esp_err_t err = rtu_transceive(ctx, req, req_len, resp, sizeof(resp), &got);
    if (err != ESP_OK) {
        return err;
    }
    if (got < sizeof(resp)) {
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != ctx->cfg.slave_id || resp[1] != 0x10 || resp[2] != req[2] || resp[3] != req[3] || resp[4] != req[4] || resp[5] != req[5]) {
        return ESP_FAIL;
    }
    uint16_t rx_crc = (uint16_t)resp[6] | ((uint16_t)resp[7] << 8U);
    uint16_t calc_crc = modbus_crc16(resp, 6);
    if (rx_crc != calc_crc) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
