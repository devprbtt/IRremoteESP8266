#pragma once

#include "esp_err.h"

#include "app_context.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telnet_server_start(app_context_t *app);
void telnet_server_logf(const char *fmt, ...);
void telnet_server_clear_logs(void);
void telnet_server_set_monitor_enabled(bool enabled);
bool telnet_server_monitor_enabled(void);
void telnet_server_request_dinplug_connect(void);
bool telnet_server_dinplug_connected(void);
typedef struct {
    char line[160];
    uint32_t seq;
} telnet_log_line_t;
size_t telnet_server_get_logs(uint32_t since_seq, telnet_log_line_t *out, size_t max_lines);

#ifdef __cplusplus
}
#endif
