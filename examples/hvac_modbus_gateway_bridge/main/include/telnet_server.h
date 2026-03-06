#pragma once

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