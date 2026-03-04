#pragma once

#include "esp_err.h"

#include "app_context.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t config_store_load(app_config_t *cfg);
esp_err_t config_store_save(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
