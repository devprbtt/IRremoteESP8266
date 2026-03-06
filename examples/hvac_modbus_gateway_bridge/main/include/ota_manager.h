#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef enum {
    OTA_TARGET_FIRMWARE = 0,
    OTA_TARGET_FILESYSTEM = 1,
} ota_target_t;

typedef struct {
    bool active;
    bool success;
    bool reboot_pending;
    ota_target_t target;
    uint32_t bytes_done;
    uint32_t bytes_total;
    int last_err;
    char url[APP_CFG_OTA_URL_LEN + 1];
    char message[96];
} ota_status_t;

typedef struct ota_manager ota_manager_t;

typedef struct {
    app_config_t *cfg;
} ota_manager_config_t;

esp_err_t ota_manager_init(ota_manager_t **out_mgr, const ota_manager_config_t *cfg);
void ota_manager_deinit(ota_manager_t *mgr);

esp_err_t ota_manager_start(ota_manager_t *mgr, ota_target_t target, const char *url);
void ota_manager_get_status(ota_manager_t *mgr, ota_status_t *out_status);

