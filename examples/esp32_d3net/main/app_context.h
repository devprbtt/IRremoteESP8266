#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "d3net_gateway.h"
#include "modbus_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    bool success;
    char message[96];
    size_t bytes_received;
    size_t total_bytes;
} ota_state_t;

typedef struct {
    char sta_ssid[33];
    char sta_password[65];
    bool sta_configured;
    bool sta_dhcp;
    char sta_ip[16];
    char sta_gateway[16];
    char sta_subnet[16];
    char sta_dns[16];
    char hostname[33];
    uint16_t telnet_port;
    char din_gateway_host[64];
    bool din_auto_connect;
    char din_actions_json[4096];
    uint64_t registered_mask;
    char registered_ids[D3NET_MAX_UNITS][6];
    modbus_rtu_config_t rtu_cfg;
} app_config_t;

typedef struct {
    d3net_gateway_t gateway;
    SemaphoreHandle_t gateway_lock;
    ota_state_t ota;
    app_config_t config;
} app_context_t;

#ifdef __cplusplus
}
#endif
