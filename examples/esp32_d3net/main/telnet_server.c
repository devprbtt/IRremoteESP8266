#include "telnet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "telnet_server";

typedef struct {
    app_context_t *app;
    uint16_t listen_port;
    int clients[4];
    struct {
        char line[160];
        uint32_t seq;
    } logs[128];
    size_t log_head;
    size_t log_count;
    uint32_t log_seq;
    bool monitor_enabled;
} telnet_ctx_t;

static telnet_ctx_t s_telnet = {
    .app = NULL,
    .listen_port = 23,
    .clients = {-1, -1, -1, -1},
    .log_head = 0,
    .log_count = 0,
    .log_seq = 0,
    .monitor_enabled = true,
};

static void telnet_broadcast(const char *line, size_t len) {
    for (size_t i = 0; i < sizeof(s_telnet.clients) / sizeof(s_telnet.clients[0]); i++) {
        int fd = s_telnet.clients[i];
        if (fd < 0) {
            continue;
        }
        int r = send(fd, line, len, 0);
        if (r < 0) {
            close(fd);
            s_telnet.clients[i] = -1;
        }
    }
}

void telnet_server_logf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }

    size_t len = (size_t)n;
    if (len >= sizeof(buf) - 2U) {
        len = sizeof(buf) - 3U;
    }
    buf[len++] = '\r';
    buf[len++] = '\n';
    if (s_telnet.monitor_enabled) {
        s_telnet.log_seq++;
        size_t slot = (s_telnet.log_head + s_telnet.log_count) % (sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0]));
        if (s_telnet.log_count == sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0])) {
            s_telnet.log_head = (s_telnet.log_head + 1U) % (sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0]));
        } else {
            s_telnet.log_count++;
        }
        memset(s_telnet.logs[slot].line, 0, sizeof(s_telnet.logs[slot].line));
        memcpy(s_telnet.logs[slot].line, buf, len < sizeof(s_telnet.logs[slot].line) ? len : sizeof(s_telnet.logs[slot].line) - 1U);
        s_telnet.logs[slot].seq = s_telnet.log_seq;
    }

    telnet_broadcast(buf, len);
}

void telnet_server_clear_logs(void) {
    s_telnet.log_head = 0;
    s_telnet.log_count = 0;
}

void telnet_server_set_monitor_enabled(bool enabled) {
    s_telnet.monitor_enabled = enabled;
}

bool telnet_server_monitor_enabled(void) {
    return s_telnet.monitor_enabled;
}

size_t telnet_server_get_logs(uint32_t since_seq, telnet_log_line_t *out, size_t max_lines) {
    size_t written = 0;
    for (size_t i = 0; i < s_telnet.log_count && written < max_lines; i++) {
        size_t idx = (s_telnet.log_head + i) % (sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0]));
        if (s_telnet.logs[idx].seq <= since_seq) {
            continue;
        }
        strncpy(out[written].line, s_telnet.logs[idx].line, sizeof(out[written].line) - 1U);
        out[written].line[sizeof(out[written].line) - 1U] = '\0';
        out[written].seq = s_telnet.logs[idx].seq;
        written++;
    }
    return written;
}

static void telnet_status_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_telnet.app != NULL && s_telnet.app->gateway_lock != NULL) {
            if (xSemaphoreTake(s_telnet.app->gateway_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                telnet_server_logf("units=%u", s_telnet.app->gateway.discovered_count);
                for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
                    d3net_unit_t *u = &s_telnet.app->gateway.units[i];
                    if (!u->present) {
                        continue;
                    }
                    telnet_server_logf(
                        "%s pwr=%d mode=%d set=%.1f cur=%.1f",
                        u->unit_id,
                        d3net_status_power_get(&u->status),
                        d3net_status_oper_mode_get(&u->status),
                        d3net_status_temp_setpoint_get(&u->status),
                        d3net_status_temp_current_get(&u->status));
                }
                xSemaphoreGive(s_telnet.app->gateway_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void telnet_accept_task(void *arg) {
    (void)arg;
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_telnet.listen_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (bind(listen_fd, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 4) != 0) {
        ESP_LOGE(TAG, "listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "telnet server listening on port %u", (unsigned)s_telnet.listen_port);

    while (true) {
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            continue;
        }

        size_t slot = sizeof(s_telnet.clients) / sizeof(s_telnet.clients[0]);
        for (size_t i = 0; i < sizeof(s_telnet.clients) / sizeof(s_telnet.clients[0]); i++) {
            if (s_telnet.clients[i] < 0) {
                slot = i;
                break;
            }
        }

        if (slot >= sizeof(s_telnet.clients) / sizeof(s_telnet.clients[0])) {
            close(sock);
            continue;
        }

        s_telnet.clients[slot] = sock;
        const char *hello = "D3Net telnet connected\r\n";
        send(sock, hello, strlen(hello), 0);
    }
}

esp_err_t telnet_server_start(app_context_t *app) {
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_telnet.app = app;
    s_telnet.listen_port = (app->config.telnet_port == 0U) ? 23U : app->config.telnet_port;
    xTaskCreate(telnet_accept_task, "telnet_accept", 4096, NULL, 5, NULL);
    xTaskCreate(telnet_status_task, "telnet_status", 4096, NULL, 4, NULL);
    return ESP_OK;
}
