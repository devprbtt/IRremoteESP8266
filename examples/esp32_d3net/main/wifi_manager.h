#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_scan_item_t;

esp_err_t wifi_manager_start_apsta(const char *ap_ssid, const char *ap_password, const char *hostname);
esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password);
esp_err_t wifi_manager_set_sta_network(bool dhcp, const char *ip, const char *gateway, const char *subnet, const char *dns);
bool wifi_manager_sta_connected(void);
esp_err_t wifi_manager_sta_ip(char *out, size_t out_len);
esp_err_t wifi_manager_scan(wifi_scan_item_t *items, size_t max_items, size_t *out_count);
