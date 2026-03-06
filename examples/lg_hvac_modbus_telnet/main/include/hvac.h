#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "modbus_client.h"

typedef struct {
    uint16_t central_address;
    uint8_t slave_id;
    bool power;
    uint8_t mode;
    uint8_t fan_speed;
    int16_t setpoint_tenths;
    int16_t room_temp_tenths;
    bool alarm;
    uint16_t error_code;
    bool connected;
    uint64_t last_update_ms;
    char last_error[64];
} hvac_zone_state_t;

typedef struct hvac_manager hvac_manager_t;

esp_err_t hvac_manager_init(hvac_manager_t **out_mgr, const app_config_t *cfg, modbus_client_t *modbus);
void hvac_manager_start(hvac_manager_t *mgr);
void hvac_manager_deinit(hvac_manager_t *mgr);

size_t hvac_manager_zone_count(const hvac_manager_t *mgr);
const hvac_zone_state_t *hvac_manager_get_zone(const hvac_manager_t *mgr, size_t index);

esp_err_t hvac_manager_refresh_zone(hvac_manager_t *mgr, size_t zone_index);
esp_err_t hvac_manager_set_field(hvac_manager_t *mgr, size_t zone_index, const char *field, const char *value, char *msg, size_t msg_len);
esp_err_t hvac_manager_zone_to_json(const hvac_manager_t *mgr, size_t zone_index, char *out, size_t out_len);
esp_err_t hvac_manager_zones_to_json(const hvac_manager_t *mgr, char *out, size_t out_len);