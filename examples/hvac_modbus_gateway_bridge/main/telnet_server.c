#include "telnet_server.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define TELNET_PORT 23
#define TELNET_TASK_STACK 6144
#define TELNET_TASK_PRIO 5
#define TELNET_MAX_LOG_LINES 128

static const char *TAG = "telnet";

typedef struct {
    char line[160];
    uint32_t seq;
} monitor_line_t;

struct telnet_server {
    app_config_t *cfg;
    hvac_manager_t *hvac;
    wifi_manager_t *wifi;
    TaskHandle_t task;
    int listen_fd;
    bool stop;
};

static struct {
    monitor_line_t lines[TELNET_MAX_LOG_LINES];
    size_t head;
    size_t count;
    uint32_t seq;
    bool enabled;
    SemaphoreHandle_t lock;
} s_monitor = {
    .head = 0,
    .count = 0,
    .seq = 0,
    .enabled = true,
    .lock = NULL,
};

static int sendf(int fd, const char *fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0) {
        return n;
    }
    if (n > (int)sizeof(line)) {
        n = (int)sizeof(line);
    }
    return send(fd, line, n, 0);
}

static void trim(char *s)
{
    if (!s) {
        return;
    }

    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }

    size_t l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) {
        s[--l] = '\0';
    }
}

void telnet_server_logf(const char *fmt, ...)
{
    char msg[256] = {0};
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }

    size_t len = (size_t)n;
    if (len > sizeof(msg) - 3U) {
        len = sizeof(msg) - 3U;
    }
    msg[len++] = '\r';
    msg[len++] = '\n';
    msg[len] = '\0';

    if (!s_monitor.enabled || s_monitor.lock == NULL) {
        return;
    }

    if (xSemaphoreTake(s_monitor.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    s_monitor.seq++;
    size_t slot = (s_monitor.head + s_monitor.count) % TELNET_MAX_LOG_LINES;
    if (s_monitor.count == TELNET_MAX_LOG_LINES) {
        s_monitor.head = (s_monitor.head + 1U) % TELNET_MAX_LOG_LINES;
    } else {
        s_monitor.count++;
    }

    memset(s_monitor.lines[slot].line, 0, sizeof(s_monitor.lines[slot].line));
    memcpy(s_monitor.lines[slot].line,
           msg,
           len < sizeof(s_monitor.lines[slot].line) ? len : (sizeof(s_monitor.lines[slot].line) - 1U));
    s_monitor.lines[slot].seq = s_monitor.seq;

    xSemaphoreGive(s_monitor.lock);
}

void telnet_server_clear_logs(void)
{
    if (s_monitor.lock == NULL) {
        return;
    }

    if (xSemaphoreTake(s_monitor.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_monitor.head = 0;
        s_monitor.count = 0;
        xSemaphoreGive(s_monitor.lock);
    }
}

void telnet_server_set_monitor_enabled(bool enabled)
{
    s_monitor.enabled = enabled;
}

bool telnet_server_monitor_enabled(void)
{
    return s_monitor.enabled;
}

size_t telnet_server_get_logs(uint32_t since_seq, telnet_log_line_t *out, size_t max_lines)
{
    if (out == NULL || max_lines == 0 || s_monitor.lock == NULL) {
        return 0;
    }

    size_t written = 0;
    if (xSemaphoreTake(s_monitor.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    for (size_t i = 0; i < s_monitor.count && written < max_lines; i++) {
        size_t idx = (s_monitor.head + i) % TELNET_MAX_LOG_LINES;
        if (s_monitor.lines[idx].seq <= since_seq) {
            continue;
        }

        memset(out[written].line, 0, sizeof(out[written].line));
        strncpy(out[written].line, s_monitor.lines[idx].line, sizeof(out[written].line) - 1U);
        out[written].seq = s_monitor.lines[idx].seq;
        written++;
    }

    xSemaphoreGive(s_monitor.lock);
    return written;
}

static bool handle_command(telnet_server_t *srv, int fd, char *line)
{
    trim(line);
    if (line[0] == '\0') {
        return true;
    }

    char original[256] = {0};
    strlcpy(original, line, sizeof(original));

    char *save = NULL;
    char *cmd = strtok_r(line, " ", &save);
    if (!cmd) {
        return true;
    }

    if (strcasecmp(cmd, "HELP") == 0) {
        sendf(fd,
              "{\"ok\":true,\"commands\":[\"help\",\"status\",\"zones\",\"get <idx>\","
              "\"read <idx>\",\"set <idx> <power|mode|fan|setpoint> <value>\","
              "\"config get\",\"config set wifi <ssid> <pass>\",\"config set zones <csv>\","
              "\"config set poll_ms <ms>\",\"config set idu_base <0|1>\",\"config set slave <1..247>\","
              "\"config set gateway <lg|midea|daikin|hitachi|samsung>\",\"config save\","
              "\"monitor status|on|off|clear\",\"reboot\",\"quit\"]}\r\n");
        return true;
    }

    if (strcasecmp(cmd, "QUIT") == 0 || strcasecmp(cmd, "EXIT") == 0) {
        sendf(fd, "{\"ok\":true,\"msg\":\"bye\"}\r\n");
        return false;
    }

    if (strcasecmp(cmd, "STATUS") == 0) {
        char ip[32] = "0.0.0.0";
        wifi_manager_get_ip(srv->wifi, ip, sizeof(ip));
        uint64_t up = (uint64_t)(esp_timer_get_time() / 1000);
        sendf(fd,
              "{\"ok\":true,\"device_id\":\"%s\",\"wifi_connected\":%s,\"ip\":\"%s\",\"uptime_ms\":%llu}\r\n",
              srv->cfg->system.device_id,
              wifi_manager_is_connected(srv->wifi) ? "true" : "false",
              ip,
              (unsigned long long)up);
        return true;
    }

    if (strcasecmp(cmd, "ZONES") == 0) {
        char json[4096];
        if (hvac_manager_zones_to_json(srv->hvac, json, sizeof(json)) == ESP_OK) {
            sendf(fd, "{\"ok\":true,\"zones\":%s}\r\n", json);
        } else {
            sendf(fd, "{\"ok\":false,\"error\":\"zones encode failed\"}\r\n");
        }
        return true;
    }

    if (strcasecmp(cmd, "GET") == 0 || strcasecmp(cmd, "READ") == 0) {
        char *idx_s = strtok_r(NULL, " ", &save);
        if (!idx_s) {
            sendf(fd, "{\"ok\":false,\"error\":\"usage: get <index>\"}\r\n");
            return true;
        }
        size_t idx = (size_t)strtoul(idx_s, NULL, 10);
        if (strcasecmp(cmd, "READ") == 0) {
            hvac_manager_refresh_zone(srv->hvac, idx);
        }
        char one[512];
        if (hvac_manager_zone_to_json(srv->hvac, idx, one, sizeof(one)) == ESP_OK) {
            sendf(fd, "{\"ok\":true,\"zone\":%s}\r\n", one);
        } else {
            sendf(fd, "{\"ok\":false,\"error\":\"invalid zone\"}\r\n");
        }
        return true;
    }

    if (strcasecmp(cmd, "SET") == 0) {
        char *idx_s = strtok_r(NULL, " ", &save);
        char *field = strtok_r(NULL, " ", &save);
        char *val = save;
        if (!idx_s || !field || !val) {
            sendf(fd, "{\"ok\":false,\"error\":\"usage: set <idx> <field> <value>\"}\r\n");
            return true;
        }
        size_t idx = (size_t)strtoul(idx_s, NULL, 10);
        trim(val);
        char msg[96];
        esp_err_t err = hvac_manager_set_field(srv->hvac, idx, field, val, msg, sizeof(msg));
        sendf(fd, "{\"ok\":%s,\"msg\":\"%s\",\"err\":%d}\r\n", (err == ESP_OK) ? "true" : "false", msg, err);
        telnet_server_logf("telnet set idx=%u field=%s value=%s result=%s", (unsigned)idx, field, val, msg);
        return true;
    }

    if (strcasecmp(cmd, "MONITOR") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        if (sub == NULL || strcasecmp(sub, "STATUS") == 0) {
            sendf(fd, "{\"ok\":true,\"enabled\":%s}\r\n", telnet_server_monitor_enabled() ? "true" : "false");
            return true;
        }

        if (strcasecmp(sub, "ON") == 0 || strcasecmp(sub, "ENABLE") == 0) {
            telnet_server_set_monitor_enabled(true);
            sendf(fd, "{\"ok\":true,\"enabled\":true}\r\n");
            return true;
        }

        if (strcasecmp(sub, "OFF") == 0 || strcasecmp(sub, "DISABLE") == 0) {
            telnet_server_set_monitor_enabled(false);
            sendf(fd, "{\"ok\":true,\"enabled\":false}\r\n");
            return true;
        }

        if (strcasecmp(sub, "CLEAR") == 0) {
            telnet_server_clear_logs();
            sendf(fd, "{\"ok\":true,\"msg\":\"cleared\"}\r\n");
            return true;
        }

        sendf(fd, "{\"ok\":false,\"error\":\"usage: monitor status|on|off|clear\"}\r\n");
        return true;
    }

    if (strcasecmp(cmd, "CONFIG") == 0) {
        char *sub = strtok_r(NULL, " ", &save);
        if (!sub) {
            sendf(fd, "{\"ok\":false,\"error\":\"usage: config get|set|save\"}\r\n");
            return true;
        }

        if (strcasecmp(sub, "GET") == 0) {
            char json[2048];
            if (app_config_to_json(srv->cfg, json, sizeof(json)) == ESP_OK) {
                sendf(fd, "%s\r\n", json);
            } else {
                sendf(fd, "{\"ok\":false,\"error\":\"config encode failed\"}\r\n");
            }
            return true;
        }

        if (strcasecmp(sub, "SAVE") == 0) {
            esp_err_t err = app_config_save(srv->cfg);
            sendf(fd, "{\"ok\":%s,\"err\":%d}\r\n", err == ESP_OK ? "true" : "false", err);
            telnet_server_logf("telnet config save result=%s", err == ESP_OK ? "ok" : "failed");
            return true;
        }

        if (strcasecmp(sub, "SET") == 0) {
            char *key = strtok_r(NULL, " ", &save);
            if (!key) {
                sendf(fd, "{\"ok\":false,\"error\":\"missing key\"}\r\n");
                return true;
            }

            if (strcasecmp(key, "WIFI") == 0) {
                char *ssid = strtok_r(NULL, " ", &save);
                char *pass = save;
                if (!ssid || !pass) {
                    sendf(fd, "{\"ok\":false,\"error\":\"usage: config set wifi <ssid> <pass>\"}\r\n");
                    return true;
                }
                trim(pass);
                app_config_set_wifi(srv->cfg, ssid, pass);
                sendf(fd, "{\"ok\":true,\"msg\":\"wifi updated (save + reboot)\"}\r\n");
                telnet_server_logf("telnet config wifi updated ssid=%s", ssid);
                return true;
            }

            if (strcasecmp(key, "ZONES") == 0) {
                char *csv = save;
                if (!csv) {
                    sendf(fd, "{\"ok\":false,\"error\":\"usage: config set zones <csv>\"}\r\n");
                    return true;
                }
                trim(csv);
                esp_err_t err = app_config_set_zones_from_csv(srv->cfg, csv);
                sendf(fd,
                      "{\"ok\":%s,\"msg\":\"zones %s (reboot to apply)\",\"err\":%d}\r\n",
                      err == ESP_OK ? "true" : "false",
                      err == ESP_OK ? "updated" : "invalid",
                      err);
                return true;
            }

            char *val = strtok_r(NULL, " ", &save);
            if (!val) {
                sendf(fd, "{\"ok\":false,\"error\":\"missing value\"}\r\n");
                return true;
            }

            if (strcasecmp(key, "POLL_MS") == 0) {
                srv->cfg->hvac.poll_interval_ms = atoi(val);
                sendf(fd, "{\"ok\":true,\"msg\":\"poll interval updated (reboot to apply)\"}\r\n");
                return true;
            }

            if (strcasecmp(key, "IDU_BASE") == 0) {
                int b = atoi(val);
                if (b != 0 && b != 1) {
                    sendf(fd, "{\"ok\":false,\"error\":\"idu_base must be 0 or 1\"}\r\n");
                } else {
                    srv->cfg->hvac.idu_address_base = b;
                    sendf(fd, "{\"ok\":true,\"msg\":\"idu base updated (reboot to apply)\"}\r\n");
                }
                return true;
            }

            if (strcasecmp(key, "SLAVE") == 0) {
                int s = atoi(val);
                if (s < 1 || s > 247) {
                    sendf(fd, "{\"ok\":false,\"error\":\"slave must be 1..247\"}\r\n");
                } else {
                    srv->cfg->modbus.default_slave_id = (uint8_t)s;
                    sendf(fd, "{\"ok\":true,\"msg\":\"default slave updated\"}\r\n");
                }
                return true;
            }

            if (strcasecmp(key, "GATEWAY") == 0) {
                if (strcasecmp(val, "midea") == 0 || strcasecmp(val, "midea_gw3_mod") == 0) {
                    srv->cfg->hvac.gateway_type = HVAC_GATEWAY_MIDEA_GW3_MOD;
                    srv->cfg->hvac.idu_address_base = 0;
                    sendf(fd, "{\"ok\":true,\"msg\":\"gateway set to midea_gw3_mod (reboot to apply)\"}\r\n");
                } else if (strcasecmp(val, "daikin") == 0 || strcasecmp(val, "daikin_dta116a51") == 0) {
                    srv->cfg->hvac.gateway_type = HVAC_GATEWAY_DAIKIN_DTA116A51;
                    srv->cfg->hvac.idu_address_base = 0;
                    sendf(fd, "{\"ok\":true,\"msg\":\"gateway set to daikin_dta116a51 (reboot to apply)\"}\r\n");
                } else if (strcasecmp(val, "hitachi") == 0 || strcasecmp(val, "hitachi_hca_mb") == 0) {
                    srv->cfg->hvac.gateway_type = HVAC_GATEWAY_HITACHI_HCA_MB;
                    srv->cfg->hvac.idu_address_base = 0;
                    sendf(fd, "{\"ok\":true,\"msg\":\"gateway set to hitachi_hca_mb (reboot to apply)\"}\r\n");
                } else if (strcasecmp(val, "samsung") == 0 || strcasecmp(val, "samsung_mim_b19n") == 0) {
                    srv->cfg->hvac.gateway_type = HVAC_GATEWAY_SAMSUNG_MIM_B19N;
                    srv->cfg->hvac.idu_address_base = 0;
                    sendf(fd, "{\"ok\":true,\"msg\":\"gateway set to samsung_mim_b19n (reboot to apply)\"}\r\n");
                } else if (strcasecmp(val, "lg") == 0 || strcasecmp(val, "lg_pmbusb00a") == 0) {
                    srv->cfg->hvac.gateway_type = HVAC_GATEWAY_LG_PMBUSB00A;
                    sendf(fd, "{\"ok\":true,\"msg\":\"gateway set to lg_pmbusb00a (reboot to apply)\"}\r\n");
                } else {
                    sendf(fd, "{\"ok\":false,\"error\":\"gateway must be lg, midea, daikin, hitachi, or samsung\"}\r\n");
                }
                return true;
            }

            sendf(fd, "{\"ok\":false,\"error\":\"unknown config key\"}\r\n");
            return true;
        }

        sendf(fd, "{\"ok\":false,\"error\":\"unknown config action\"}\r\n");
        return true;
    }

    if (strcasecmp(cmd, "REBOOT") == 0) {
        sendf(fd, "{\"ok\":true,\"msg\":\"rebooting\"}\r\n");
        telnet_server_logf("telnet reboot requested");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return false;
    }

    sendf(fd, "{\"ok\":false,\"error\":\"unknown command\"}\r\n");
    telnet_server_logf("telnet unknown command: %s", original);
    return true;
}

static void client_session(telnet_server_t *srv, int fd)
{
    sendf(fd, "Welcome to HVAC Modbus Gateway Telnet API. Type HELP.\\r\\n");
    char line[256];
    size_t len = 0;

    while (!srv->stop) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) {
            return;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            bool keep = handle_command(srv, fd, line);
            len = 0;
            if (!keep) {
                return;
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

static void telnet_task(void *arg)
{
    telnet_server_t *srv = (telnet_server_t *)arg;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (srv->listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TELNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv->listen_fd, 2) != 0) {
        ESP_LOGE(TAG, "bind/listen failed errno=%d", errno);
        close(srv->listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "telnet server listening on port %d", TELNET_PORT);
    telnet_server_logf("telnet server listening on port %d", TELNET_PORT);

    while (!srv->stop) {
        struct sockaddr_in source_addr;
        socklen_t alen = sizeof(source_addr);
        int fd = accept(srv->listen_fd, (struct sockaddr *)&source_addr, &alen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "telnet client connected");
        telnet_server_logf("telnet client connected");
        client_session(srv, fd);
        close(fd);
        ESP_LOGI(TAG, "telnet client disconnected");
        telnet_server_logf("telnet client disconnected");
    }

    close(srv->listen_fd);
    vTaskDelete(NULL);
}

esp_err_t telnet_server_start(telnet_server_t **out_server, const telnet_server_config_t *cfg)
{
    if (!out_server || !cfg || !cfg->cfg || !cfg->hvac || !cfg->wifi) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_monitor.lock == NULL) {
        s_monitor.lock = xSemaphoreCreateMutex();
        if (s_monitor.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    telnet_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        return ESP_ERR_NO_MEM;
    }

    srv->cfg = cfg->cfg;
    srv->hvac = cfg->hvac;
    srv->wifi = cfg->wifi;

    xTaskCreate(telnet_task, "telnet_srv", TELNET_TASK_STACK, srv, TELNET_TASK_PRIO, &srv->task);
    *out_server = srv;
    return ESP_OK;
}

void telnet_server_stop(telnet_server_t *server)
{
    if (!server) {
        return;
    }

    server->stop = true;
    if (server->listen_fd > 0) {
        close(server->listen_fd);
    }

    free(server);
}
