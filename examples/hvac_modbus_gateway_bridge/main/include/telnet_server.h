#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "hvac.h"
#include "wifi_manager.h"

typedef struct {
    app_config_t *cfg;
    hvac_manager_t *hvac;
    wifi_manager_t *wifi;
} telnet_server_config_t;

typedef struct telnet_server telnet_server_t;

esp_err_t telnet_server_start(telnet_server_t **out_server, const telnet_server_config_t *cfg);
void telnet_server_stop(telnet_server_t *server);
void telnet_server_logf(const char *fmt, ...);
void telnet_server_clear_logs(void);
void telnet_server_set_monitor_enabled(bool enabled);
bool telnet_server_monitor_enabled(void);

typedef struct {
    char line[160];
    uint32_t seq;
} telnet_log_line_t;

size_t telnet_server_get_logs(uint32_t since_seq, telnet_log_line_t *out, size_t max_lines);
