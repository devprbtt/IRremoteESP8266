#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "d3net_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int de_pin;
    int re_pin;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t stop_bits;
    char parity;  // 'N', 'E', 'O'
    uint8_t slave_id;
    uint32_t timeout_ms;
} modbus_rtu_config_t;

typedef struct {
    modbus_rtu_config_t cfg;
    bool initialized;
} modbus_rtu_ctx_t;

esp_err_t modbus_rtu_init(modbus_rtu_ctx_t *ctx, const modbus_rtu_config_t *cfg);

esp_err_t modbus_rtu_read_registers(
    modbus_rtu_ctx_t *ctx,
    d3net_reg_type_t reg_type,
    uint16_t address,
    uint16_t count,
    uint16_t *out_words);

esp_err_t modbus_rtu_write_registers(
    modbus_rtu_ctx_t *ctx,
    uint16_t address,
    uint16_t count,
    const uint16_t *words);

#ifdef __cplusplus
}
#endif
