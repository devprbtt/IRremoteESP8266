#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_CFG_MAX_ZONES 16
#define APP_CFG_DEVICE_ID_LEN 32
#define APP_CFG_WIFI_SSID_LEN 32
#define APP_CFG_WIFI_PASS_LEN 64
#define APP_CFG_OTA_URL_LEN 192

typedef struct {
    uint8_t slave_id;
    uint16_t central_address;
} zone_cfg_t;

typedef enum {
    HVAC_GATEWAY_LG_PMBUSB00A = 0,
    HVAC_GATEWAY_MIDEA_GW3_MOD = 1,
    HVAC_GATEWAY_DAIKIN_DTA116A51 = 2,
    HVAC_GATEWAY_HITACHI_HCA_MB = 3,
    HVAC_GATEWAY_SAMSUNG_MIM_B19N = 4,
    HVAC_GATEWAY_GREE_GMV = 5,
} hvac_gateway_type_t;

typedef struct {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int de_pin;
    int baud;
    int parity;
    int stop_bits;
    int timeout_ms;
    int retries;
    uint8_t default_slave_id;
    bool use_hw_rs485;
} modbus_cfg_t;

typedef struct {
    hvac_gateway_type_t gateway_type;
    int idu_address_base;
    int poll_interval_ms;
    int mode_rate_limit_ms;
    int setpoint_min_tenths;
    int setpoint_max_tenths;
    size_t zone_count;
    zone_cfg_t zones[APP_CFG_MAX_ZONES];
} hvac_cfg_t;

typedef struct {
    char device_id[APP_CFG_DEVICE_ID_LEN + 1];
    int log_level;
} system_cfg_t;

typedef struct {
    char ssid[APP_CFG_WIFI_SSID_LEN + 1];
    char password[APP_CFG_WIFI_PASS_LEN + 1];
} wifi_cfg_t;

typedef struct {
    system_cfg_t system;
    wifi_cfg_t wifi;
    modbus_cfg_t modbus;
    hvac_cfg_t hvac;
    char ota_firmware_url[APP_CFG_OTA_URL_LEN + 1];
    char ota_filesystem_url[APP_CFG_OTA_URL_LEN + 1];
} app_config_t;

void app_config_set_defaults(app_config_t *cfg);
esp_err_t app_config_load(app_config_t *cfg);
esp_err_t app_config_save(const app_config_t *cfg);
esp_err_t app_config_set_wifi(app_config_t *cfg, const char *ssid, const char *password);
esp_err_t app_config_set_zones_from_csv(app_config_t *cfg, const char *csv);
esp_err_t app_config_get_zones_csv(const app_config_t *cfg, char *out, size_t out_len);
esp_err_t app_config_to_json(const app_config_t *cfg, char *out, size_t out_len);
