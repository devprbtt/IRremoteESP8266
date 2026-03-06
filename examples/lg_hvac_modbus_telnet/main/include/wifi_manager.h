#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct wifi_manager wifi_manager_t;

typedef struct {
    app_config_t *cfg;
} wifi_manager_config_t;

esp_err_t wifi_manager_init(wifi_manager_t **out_mgr, const wifi_manager_config_t *cfg);
bool wifi_manager_is_connected(const wifi_manager_t *mgr);
esp_err_t wifi_manager_get_ip(const wifi_manager_t *mgr, char *buf, size_t buf_len);
bool wifi_manager_is_ap_mode(const wifi_manager_t *mgr);
esp_err_t wifi_manager_get_ap_ssid(const wifi_manager_t *mgr, char *buf, size_t buf_len);
