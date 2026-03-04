#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include "app_context.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(app_context_t *app, httpd_handle_t *out_handle);

#ifdef __cplusplus
}
#endif
