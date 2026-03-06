#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "hvac.h"
#include "wifi_manager.h"

typedef struct web_server web_server_t;

typedef struct {
    app_config_t *cfg;
    hvac_manager_t *hvac;
    wifi_manager_t *wifi;
} web_server_config_t;

esp_err_t web_server_start(web_server_t **out_server, const web_server_config_t *cfg);
void web_server_stop(web_server_t *server);

