#include "modbus_client.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "modbus_crc.h"

#define MB_MAX_ADU 256

static const char *TAG = "modbus_client";

struct modbus_client {
    modbus_client_config_t cfg;
    SemaphoreHandle_t lock;
    bool manual_de;
    uint8_t last_exception;
};

static int interframe_delay_us(int baud)
{
    int us = (int)(38500000LL / (baud > 0 ? baud : 9600));
    if (us < 1750) {
        us = 1750;
    }
    return us;
}

static uart_parity_t parity_from_cfg(int parity)
{
    switch (parity) {
        case 1:
            return UART_PARITY_EVEN;
        case 2:
            return UART_PARITY_ODD;
        case 0:
        default:
            return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t stop_bits_from_cfg(int stop_bits)
{
    switch (stop_bits) {
        case 2:
            return UART_STOP_BITS_2;
        case 1:
        default:
            return UART_STOP_BITS_1;
    }
}

static esp_err_t uart_read_exact(uart_port_t uart, uint8_t *buf, size_t want, int timeout_ms)
{
    size_t got = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (got < want) {
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - start;
        if (elapsed >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        TickType_t left = timeout_ticks - elapsed;
        int r = uart_read_bytes(uart, buf + got, want - got, left);
        if (r < 0) {
            return ESP_FAIL;
        }
        got += (size_t)r;
    }

    return ESP_OK;
}

static esp_err_t transact_once(modbus_client_t *c, const uint8_t *req, size_t req_len, uint8_t *rsp, size_t *rsp_len)
{
    ESP_RETURN_ON_FALSE(c && req && rsp && rsp_len, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    uart_flush_input(c->cfg.uart_num);
    esp_rom_delay_us(interframe_delay_us(c->cfg.baud));

    if (c->manual_de) {
        gpio_set_level(c->cfg.de_pin, 1);
    }

    int written = uart_write_bytes(c->cfg.uart_num, req, req_len);
    if (written != (int)req_len) {
        if (c->manual_de) {
            gpio_set_level(c->cfg.de_pin, 0);
        }
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(uart_wait_tx_done(c->cfg.uart_num, pdMS_TO_TICKS(c->cfg.timeout_ms)), TAG, "tx wait failed");

    if (c->manual_de) {
        gpio_set_level(c->cfg.de_pin, 0);
    }

    uint8_t head[3] = {0};
    ESP_RETURN_ON_ERROR(uart_read_exact(c->cfg.uart_num, head, sizeof(head), c->cfg.timeout_ms), TAG, "timeout header");

    rsp[0] = head[0];
    rsp[1] = head[1];
    rsp[2] = head[2];

    size_t total = 0;
    if ((head[1] & 0x80) != 0) {
        total = 5;
    } else if (head[1] == 0x05 || head[1] == 0x06 || head[1] == 0x10) {
        total = 8;
    } else {
        total = (size_t)(3 + head[2] + 2);
    }

    if (total > MB_MAX_ADU || total < 5) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(uart_read_exact(c->cfg.uart_num, rsp + 3, total - 3, c->cfg.timeout_ms), TAG, "timeout body");

    uint16_t crc_calc = modbus_crc16(rsp, total - 2);
    uint16_t crc_rx = (uint16_t)rsp[total - 2] | ((uint16_t)rsp[total - 1] << 8);
    if (crc_calc != crc_rx) {
        return ESP_ERR_INVALID_CRC;
    }

    *rsp_len = total;
    return ESP_OK;
}

static esp_err_t transact(modbus_client_t *c, const uint8_t *req, size_t req_len, uint8_t *rsp, size_t *rsp_len)
{
    int attempts = c->cfg.retries;
    if (attempts < 1) {
        attempts = 1;
    }

    for (int attempt = 0; attempt < attempts; attempt++) {
        esp_err_t err = transact_once(c, req, req_len, rsp, rsp_len);
        if (err == ESP_OK) {
            if ((rsp[1] & 0x80) != 0) {
                c->last_exception = rsp[2];
                ESP_LOGW(TAG, "modbus exception fc=0x%02X code=0x%02X", rsp[1], rsp[2]);
                return ESP_ERR_INVALID_RESPONSE;
            }
            c->last_exception = 0;
            return ESP_OK;
        }

        int backoff = 50 << attempt;
        ESP_LOGW(TAG, "modbus txn failed attempt=%d/%d err=0x%x", attempt + 1, attempts, err);
        vTaskDelay(pdMS_TO_TICKS(backoff));
    }

    return ESP_FAIL;
}

static esp_err_t do_request(modbus_client_t *c, uint8_t slave, uint8_t fc, uint16_t addr, uint16_t qty_or_value, bool is_write, uint8_t *rsp, size_t *rsp_len)
{
    uint8_t req[8] = {0};
    req[0] = slave;
    req[1] = fc;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(qty_or_value >> 8);
    req[5] = (uint8_t)(qty_or_value & 0xFF);
    if (is_write && fc == 0x05) {
        req[4] = qty_or_value ? 0xFF : 0x00;
        req[5] = 0x00;
    }

    uint16_t crc = modbus_crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    if (xSemaphoreTake(c->lock, pdMS_TO_TICKS(c->cfg.timeout_ms * 4)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = transact(c, req, sizeof(req), rsp, rsp_len);
    xSemaphoreGive(c->lock);
    return err;
}

esp_err_t modbus_client_init(modbus_client_t **out_client, const modbus_client_config_t *cfg)
{
    if (!out_client || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;

    modbus_client_t *c = calloc(1, sizeof(*c));
    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "no mem");

    c->cfg = *cfg;
    c->lock = xSemaphoreCreateMutex();
    if (!c->lock) {
        free(c);
        return ESP_ERR_NO_MEM;
    }

    uart_config_t ucfg = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = parity_from_cfg(cfg->parity),
        .stop_bits = stop_bits_from_cfg(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ret = uart_driver_install(cfg->uart_num, 1024, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: 0x%x", ret);
        goto fail;
    }
    ret = uart_param_config(cfg->uart_num, &ucfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: 0x%x", ret);
        goto fail;
    }

    esp_err_t pin_err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->de_pin, UART_PIN_NO_CHANGE);
    if (pin_err != ESP_OK) {
        ret = pin_err;
        ESP_LOGE(TAG, "uart_set_pin failed: 0x%x", ret);
        goto fail;
    }

    c->manual_de = !cfg->use_hw_rs485;
    if (cfg->use_hw_rs485) {
        esp_err_t m = uart_set_mode(cfg->uart_num, UART_MODE_RS485_HALF_DUPLEX);
        if (m != ESP_OK) {
            ESP_LOGW(TAG, "RS485 half duplex mode unavailable, using manual DE control");
            c->manual_de = true;
        }
    }

    if (c->manual_de) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->de_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ret = gpio_config(&io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed: 0x%x", ret);
            goto fail;
        }
        gpio_set_level(cfg->de_pin, 0);
    }

    *out_client = c;
    ESP_LOGI(TAG, "modbus client ready uart=%d baud=%d parity=%d stop_bits=%d", cfg->uart_num, cfg->baud, cfg->parity, cfg->stop_bits);
    return ESP_OK;

fail:
    if (c->lock) {
        vSemaphoreDelete(c->lock);
    }
    uart_driver_delete(cfg->uart_num);
    free(c);
    return (ret == ESP_OK) ? ESP_FAIL : ret;
}

void modbus_client_deinit(modbus_client_t *client)
{
    if (!client) {
        return;
    }
    uart_driver_delete(client->cfg.uart_num);
    if (client->lock) {
        vSemaphoreDelete(client->lock);
    }
    free(client);
}

esp_err_t modbus_read_coils(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint8_t *dest, size_t dest_len)
{
    if (!client || !dest || qty == 0 || qty > 2000 || dest_len < ((qty + 7) / 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x01, addr, qty, false, rsp, &rsp_len), TAG, "read coils failed");
    if (rsp[2] != (uint8_t)((qty + 7) / 8)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(dest, &rsp[3], rsp[2]);
    return ESP_OK;
}

esp_err_t modbus_read_discrete(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint8_t *dest, size_t dest_len)
{
    if (!client || !dest || qty == 0 || qty > 2000 || dest_len < ((qty + 7) / 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x02, addr, qty, false, rsp, &rsp_len), TAG, "read discrete failed");
    if (rsp[2] != (uint8_t)((qty + 7) / 8)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(dest, &rsp[3], rsp[2]);
    return ESP_OK;
}

esp_err_t modbus_read_holding(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint16_t *dest, size_t dest_len)
{
    if (!client || !dest || qty == 0 || qty > 125 || dest_len < qty) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x03, addr, qty, false, rsp, &rsp_len), TAG, "read holding failed");
    if (rsp[2] != qty * 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (uint16_t i = 0; i < qty; i++) {
        dest[i] = ((uint16_t)rsp[3 + i * 2] << 8) | rsp[4 + i * 2];
    }
    return ESP_OK;
}

esp_err_t modbus_read_input(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint16_t *dest, size_t dest_len)
{
    if (!client || !dest || qty == 0 || qty > 125 || dest_len < qty) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x04, addr, qty, false, rsp, &rsp_len), TAG, "read input failed");
    if (rsp[2] != qty * 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (uint16_t i = 0; i < qty; i++) {
        dest[i] = ((uint16_t)rsp[3 + i * 2] << 8) | rsp[4 + i * 2];
    }
    return ESP_OK;
}

esp_err_t modbus_write_single_coil(modbus_client_t *client, uint8_t slave, uint16_t addr, bool value)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x05, addr, value ? 1 : 0, true, rsp, &rsp_len), TAG, "write coil failed");
    uint16_t echoed_addr = ((uint16_t)rsp[2] << 8) | rsp[3];
    if (echoed_addr != addr) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t modbus_write_single_register(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t value)
{
    if (!client) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;
    ESP_RETURN_ON_ERROR(do_request(client, slave, 0x06, addr, value, true, rsp, &rsp_len), TAG, "write reg failed");
    uint16_t echoed_addr = ((uint16_t)rsp[2] << 8) | rsp[3];
    uint16_t echoed_value = ((uint16_t)rsp[4] << 8) | rsp[5];
    if (echoed_addr != addr || echoed_value != value) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

uint8_t modbus_client_get_last_exception(const modbus_client_t *client)
{
    if (!client) {
        return 0;
    }
    return client->last_exception;
}

esp_err_t modbus_write_multiple_registers(modbus_client_t *client, uint8_t slave, uint16_t addr, const uint16_t *values, uint16_t qty)
{
    if (!client || !values || qty == 0 || qty > 123) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t req[MB_MAX_ADU] = {0};
    size_t req_len = (size_t)(9 + qty * 2);
    if (req_len > MB_MAX_ADU) {
        return ESP_ERR_INVALID_SIZE;
    }

    req[0] = slave;
    req[1] = 0x10;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(qty >> 8);
    req[5] = (uint8_t)(qty & 0xFF);
    req[6] = (uint8_t)(qty * 2);
    for (uint16_t i = 0; i < qty; i++) {
        req[7 + i * 2] = (uint8_t)(values[i] >> 8);
        req[8 + i * 2] = (uint8_t)(values[i] & 0xFF);
    }

    uint16_t crc = modbus_crc16(req, req_len - 2);
    req[req_len - 2] = (uint8_t)(crc & 0xFF);
    req[req_len - 1] = (uint8_t)(crc >> 8);

    uint8_t rsp[MB_MAX_ADU] = {0};
    size_t rsp_len = 0;

    if (xSemaphoreTake(client->lock, pdMS_TO_TICKS(client->cfg.timeout_ms * 4)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = transact(client, req, req_len, rsp, &rsp_len);
    xSemaphoreGive(client->lock);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t echoed_addr = ((uint16_t)rsp[2] << 8) | rsp[3];
    uint16_t echoed_qty = ((uint16_t)rsp[4] << 8) | rsp[5];
    if (echoed_addr != addr || echoed_qty != qty) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
