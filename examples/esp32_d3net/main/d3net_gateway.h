#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "d3net_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D3NET_MAX_UNITS 64

#define D3NET_DEFAULT_POLL_INTERVAL_S 10U
#define D3NET_DEFAULT_THROTTLE_MS 25U
#define D3NET_DEFAULT_CACHE_WRITE_S 35U
#define D3NET_DEFAULT_CACHE_ERROR_S 10U

typedef esp_err_t (*d3net_read_registers_fn)(
    void *ctx,
    d3net_reg_type_t reg_type,
    uint16_t address,
    uint16_t count,
    uint16_t *out_words);

typedef esp_err_t (*d3net_write_registers_fn)(
    void *ctx,
    uint16_t address,
    uint16_t count,
    const uint16_t *words);

typedef struct {
    bool present;
    uint8_t index;
    char unit_id[6];
    uint64_t last_error_read_ms;
    d3net_unit_cap_t cap;
    d3net_unit_status_t status;
    d3net_unit_holding_t holding;
    d3net_unit_error_t error;
} d3net_unit_t;

typedef struct {
    d3net_read_registers_fn read_fn;
    d3net_write_registers_fn write_fn;
    void *io_ctx;
    uint8_t modbus_device_id;
    uint32_t poll_interval_s;
    uint32_t throttle_ms;
    uint32_t cache_write_s;
    uint32_t cache_error_s;
    uint64_t last_op_ms;
    d3net_system_status_t system_status;
    d3net_unit_t units[D3NET_MAX_UNITS];
    uint8_t discovered_count;
} d3net_gateway_t;

void d3net_gateway_init(
    d3net_gateway_t *gw,
    d3net_read_registers_fn read_fn,
    d3net_write_registers_fn write_fn,
    void *io_ctx,
    uint8_t modbus_device_id);

esp_err_t d3net_gateway_discover_units(d3net_gateway_t *gw);
esp_err_t d3net_gateway_poll_status(d3net_gateway_t *gw);
esp_err_t d3net_gateway_read_error(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms);

esp_err_t d3net_unit_prepare_write(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms);
esp_err_t d3net_unit_commit_write(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms);

esp_err_t d3net_unit_set_power(d3net_gateway_t *gw, d3net_unit_t *unit, bool on, uint64_t now_ms);
esp_err_t d3net_unit_set_mode(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_mode_t mode, uint64_t now_ms);
esp_err_t d3net_unit_set_setpoint(d3net_gateway_t *gw, d3net_unit_t *unit, float celsius, uint64_t now_ms);
esp_err_t d3net_unit_set_fan_speed(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_fan_speed_t speed, uint64_t now_ms);
esp_err_t d3net_unit_set_fan_dir(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_fan_dir_t dir, uint64_t now_ms);
esp_err_t d3net_unit_filter_reset(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
