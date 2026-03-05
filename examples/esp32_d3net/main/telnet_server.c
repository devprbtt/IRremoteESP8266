#include "telnet_server.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "telnet_server";
static const uint16_t DINPLUG_PORT = 23;
static const uint32_t DINPLUG_RECONNECT_MS = 5000U;
static const uint32_t DINPLUG_KEEPALIVE_MS = 10000U;

typedef struct {
    app_context_t *app;
    uint16_t listen_port;
    int clients[4];
    char client_buf[4][512];
    size_t client_buf_len[4];
    int dinplug_fd;
    bool dinplug_connected;
    uint64_t dinplug_last_attempt_ms;
    uint64_t dinplug_last_keepalive_ms;
    bool dinplug_force_connect;
    char dinplug_buf[512];
    size_t dinplug_buf_len;
    struct {
        char line[160];
        uint32_t seq;
    } logs[128];
    size_t log_head;
    size_t log_count;
    uint32_t log_seq;
    bool monitor_enabled;
    SemaphoreHandle_t log_lock;
    bool unit_power_valid[D3NET_MAX_UNITS];
    bool unit_power_cache[D3NET_MAX_UNITS];
} telnet_ctx_t;

static telnet_ctx_t s_telnet = {
    .app = NULL,
    .listen_port = 23,
    .clients = {-1, -1, -1, -1},
    .dinplug_fd = -1,
    .dinplug_connected = false,
    .dinplug_last_attempt_ms = 0,
    .dinplug_last_keepalive_ms = 0,
    .dinplug_force_connect = false,
    .dinplug_buf = {0},
    .dinplug_buf_len = 0,
    .log_head = 0,
    .log_count = 0,
    .log_seq = 0,
    .monitor_enabled = true,
    .log_lock = NULL,
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
    if (s_telnet.monitor_enabled && s_telnet.log_lock != NULL &&
        xSemaphoreTake(s_telnet.log_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_telnet.log_seq++;
        size_t slot = (s_telnet.log_head + s_telnet.log_count) % (sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0]));
        if (s_telnet.log_count == sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0])) {
            s_telnet.log_head = (s_telnet.log_head + 1U) % (sizeof(s_telnet.logs) / sizeof(s_telnet.logs[0]));
        } else {
            s_telnet.log_count++;
        }
        memset(s_telnet.logs[slot].line, 0, sizeof(s_telnet.logs[slot].line));
        memcpy(
            s_telnet.logs[slot].line,
            buf,
            len < sizeof(s_telnet.logs[slot].line) ? len : sizeof(s_telnet.logs[slot].line) - 1U);
        s_telnet.logs[slot].seq = s_telnet.log_seq;
        xSemaphoreGive(s_telnet.log_lock);
    }

    telnet_broadcast(buf, len);
}

void telnet_server_clear_logs(void) {
    if (s_telnet.log_lock != NULL && xSemaphoreTake(s_telnet.log_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_telnet.log_head = 0;
        s_telnet.log_count = 0;
        xSemaphoreGive(s_telnet.log_lock);
    }
}

void telnet_server_set_monitor_enabled(bool enabled) {
    s_telnet.monitor_enabled = enabled;
}

bool telnet_server_monitor_enabled(void) {
    return s_telnet.monitor_enabled;
}

void telnet_server_request_dinplug_connect(void) {
    s_telnet.dinplug_force_connect = true;
}

bool telnet_server_dinplug_connected(void) {
    return s_telnet.dinplug_connected;
}

static void send_json_line_fd(int fd, cJSON *root) {
    char *out = cJSON_PrintUnformatted(root);
    if (out == NULL) {
        return;
    }
    send(fd, out, strlen(out), 0);
    send(fd, "\r\n", 2, 0);
    free(out);
}

static d3net_unit_t *find_unit_by_index(int index) {
    if (s_telnet.app == NULL || index < 0 || index >= D3NET_MAX_UNITS) {
        return NULL;
    }
    d3net_unit_t *u = &s_telnet.app->gateway.units[index];
    if (!u->present) {
        return NULL;
    }
    return u;
}

static d3net_unit_t *find_unit_by_id(const char *id) {
    if (s_telnet.app == NULL || id == NULL || id[0] == '\0') {
        return NULL;
    }
    for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
        d3net_unit_t *u = &s_telnet.app->gateway.units[i];
        if (!u->present) {
            continue;
        }
        if (strcmp(u->unit_id, id) == 0) {
            return u;
        }
    }
    return NULL;
}

static d3net_mode_t mode_from_text(const char *mode) {
    if (mode == NULL) return D3NET_MODE_AUTO;
    if (strcasecmp(mode, "fan") == 0) return D3NET_MODE_FAN;
    if (strcasecmp(mode, "heat") == 0) return D3NET_MODE_HEAT;
    if (strcasecmp(mode, "cool") == 0) return D3NET_MODE_COOL;
    if (strcasecmp(mode, "auto") == 0) return D3NET_MODE_AUTO;
    if (strcasecmp(mode, "vent") == 0) return D3NET_MODE_VENT;
    if (strcasecmp(mode, "dry") == 0) return D3NET_MODE_DRY;
    return D3NET_MODE_AUTO;
}

static d3net_fan_speed_t fan_from_text(const char *fan) {
    if (fan == NULL) return D3NET_FAN_SPEED_AUTO;
    if (strcasecmp(fan, "low") == 0) return D3NET_FAN_SPEED_LOW;
    if (strcasecmp(fan, "low_medium") == 0 || strcasecmp(fan, "low-med") == 0) return D3NET_FAN_SPEED_LOW_MEDIUM;
    if (strcasecmp(fan, "medium") == 0 || strcasecmp(fan, "med") == 0) return D3NET_FAN_SPEED_MEDIUM;
    if (strcasecmp(fan, "high_medium") == 0 || strcasecmp(fan, "hi-med") == 0) return D3NET_FAN_SPEED_HIGH_MEDIUM;
    if (strcasecmp(fan, "high") == 0) return D3NET_FAN_SPEED_HIGH;
    return D3NET_FAN_SPEED_AUTO;
}

static d3net_fan_dir_t fan_dir_from_text(const char *dir) {
    if (dir == NULL) return D3NET_FAN_DIR_SWING;
    if (strcasecmp(dir, "p0") == 0) return D3NET_FAN_DIR_P0;
    if (strcasecmp(dir, "p1") == 0) return D3NET_FAN_DIR_P1;
    if (strcasecmp(dir, "p2") == 0) return D3NET_FAN_DIR_P2;
    if (strcasecmp(dir, "p3") == 0) return D3NET_FAN_DIR_P3;
    if (strcasecmp(dir, "p4") == 0) return D3NET_FAN_DIR_P4;
    if (strcasecmp(dir, "stop") == 0) return D3NET_FAN_DIR_STOP;
    return D3NET_FAN_DIR_SWING;
}

static bool send_dinplug_command(const char *cmd) {
    if (cmd == NULL || cmd[0] == '\0' || !s_telnet.dinplug_connected || s_telnet.dinplug_fd < 0) {
        return false;
    }
    send(s_telnet.dinplug_fd, cmd, strlen(cmd), 0);
    send(s_telnet.dinplug_fd, "\r\n", 2, 0);
    telnet_server_logf("dinplug tx: %s", cmd);
    return true;
}

static bool set_dinplug_led(uint16_t keypad_id, uint16_t led_id, uint8_t state) {
    if (keypad_id == 0 || led_id == 0 || state > 3U) {
        return false;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "LED %u %u %u", (unsigned)keypad_id, (unsigned)led_id, (unsigned)state);
    return send_dinplug_command(cmd);
}

static uint8_t led_follow_mode_from_binding(cJSON *binding) {
    if (binding == NULL || !cJSON_IsObject(binding)) {
        return 0;
    }
    cJSON *mode = cJSON_GetObjectItem(binding, "led_follow_mode");
    if (cJSON_IsNumber(mode)) {
        int m = mode->valueint;
        if (m >= 0 && m <= 2) {
            return (uint8_t)m;
        }
        return 0;
    }
    // Backward compatibility with old boolean field.
    cJSON *legacy = cJSON_GetObjectItem(binding, "led_follow_power");
    if (cJSON_IsTrue(legacy)) {
        return 2;
    }
    return 0;
}

static void sync_leds_for_unit_entry(cJSON *entry, uint16_t unit_index, bool power_on) {
    if (!cJSON_IsObject(entry)) {
        return;
    }
    cJSON *idx = cJSON_GetObjectItem(entry, "unit_index");
    if (!cJSON_IsNumber(idx) || idx->valueint != (int)unit_index) {
        return;
    }

    cJSON *keypads = cJSON_GetObjectItem(entry, "keypads");
    cJSON *buttons = cJSON_GetObjectItem(entry, "buttons");
    if (!cJSON_IsArray(buttons)) {
        return;
    }

    cJSON *b = NULL;
    cJSON_ArrayForEach(b, buttons) {
        if (!cJSON_IsObject(b)) {
            continue;
        }
        cJSON *kid = cJSON_GetObjectItem(b, "keypad_id");
        cJSON *bid = cJSON_GetObjectItem(b, "id");
        if (!cJSON_IsNumber(bid) || bid->valueint <= 0) {
            continue;
        }
        uint8_t mode = led_follow_mode_from_binding(b);
        if (mode == 0) {
            continue;
        }
        const uint8_t led_state = (mode == 1) ? (power_on ? 1U : 0U) : (power_on ? 0U : 1U);
        if (cJSON_IsNumber(kid) && kid->valueint > 0) {
            set_dinplug_led((uint16_t)kid->valueint, (uint16_t)bid->valueint, led_state);
            continue;
        }
        if (!cJSON_IsArray(keypads)) {
            continue;
        }
        cJSON *k = NULL;
        cJSON_ArrayForEach(k, keypads) {
            if (cJSON_IsNumber(k) && k->valueint > 0) {
                set_dinplug_led((uint16_t)k->valueint, (uint16_t)bid->valueint, led_state);
            }
        }
    }
}

static void sync_leds_for_unit_index_locked(uint16_t unit_index) {
    if (s_telnet.app == NULL || !s_telnet.dinplug_connected || s_telnet.app->config.din_actions_json[0] == '\0') {
        return;
    }
    d3net_unit_t *u = find_unit_by_index((int)unit_index);
    if (u == NULL) {
        return;
    }
    const bool power_on = d3net_status_power_get(&u->status);
    cJSON *maps = cJSON_Parse(s_telnet.app->config.din_actions_json);
    if (maps == NULL || !cJSON_IsArray(maps)) {
        cJSON_Delete(maps);
        return;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, maps) {
        sync_leds_for_unit_entry(entry, unit_index, power_on);
    }
    cJSON_Delete(maps);
}

static void sync_leds_for_all_units_locked(void) {
    if (s_telnet.app == NULL || !s_telnet.dinplug_connected || s_telnet.app->config.din_actions_json[0] == '\0') {
        return;
    }
    cJSON *maps = cJSON_Parse(s_telnet.app->config.din_actions_json);
    if (maps == NULL || !cJSON_IsArray(maps)) {
        cJSON_Delete(maps);
        return;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, maps) {
        cJSON *idx = cJSON_GetObjectItem(entry, "unit_index");
        if (!cJSON_IsNumber(idx) || idx->valueint < 0 || idx->valueint >= D3NET_MAX_UNITS) {
            continue;
        }
        d3net_unit_t *u = find_unit_by_index(idx->valueint);
        if (u == NULL) {
            continue;
        }
        sync_leds_for_unit_entry(entry, (uint16_t)idx->valueint, d3net_status_power_get(&u->status));
    }
    cJSON_Delete(maps);
}

static esp_err_t apply_action_to_unit(d3net_unit_t *u, const char *action, float value) {
    if (s_telnet.app == NULL || u == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    if (strcmp(action, "none") == 0) return ESP_OK;
    if (strcmp(action, "temp_up") == 0) return d3net_unit_set_setpoint(&s_telnet.app->gateway, u, d3net_status_temp_setpoint_get(&u->status) + value, now_ms);
    if (strcmp(action, "temp_down") == 0) return d3net_unit_set_setpoint(&s_telnet.app->gateway, u, d3net_status_temp_setpoint_get(&u->status) - value, now_ms);
    if (strcmp(action, "set_temp") == 0) return d3net_unit_set_setpoint(&s_telnet.app->gateway, u, value, now_ms);
    if (strcmp(action, "power_on") == 0) return d3net_unit_set_power(&s_telnet.app->gateway, u, true, now_ms);
    if (strcmp(action, "power_off") == 0) return d3net_unit_set_power(&s_telnet.app->gateway, u, false, now_ms);
    if (strcmp(action, "toggle_power") == 0) return d3net_unit_set_power(&s_telnet.app->gateway, u, !d3net_status_power_get(&u->status), now_ms);
    if (strcmp(action, "mode_heat") == 0) return d3net_unit_set_mode(&s_telnet.app->gateway, u, D3NET_MODE_HEAT, now_ms);
    if (strcmp(action, "mode_cool") == 0) return d3net_unit_set_mode(&s_telnet.app->gateway, u, D3NET_MODE_COOL, now_ms);
    if (strcmp(action, "mode_fan") == 0) return d3net_unit_set_mode(&s_telnet.app->gateway, u, D3NET_MODE_FAN, now_ms);
    if (strcmp(action, "mode_auto") == 0) return d3net_unit_set_mode(&s_telnet.app->gateway, u, D3NET_MODE_AUTO, now_ms);
    if (strcmp(action, "mode_off") == 0) return d3net_unit_set_power(&s_telnet.app->gateway, u, false, now_ms);
    return ESP_ERR_NOT_SUPPORTED;
}

static void process_telnet_json(int fd, const char *line) {
    cJSON *json = cJSON_Parse(line);
    if (json == NULL) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "invalid_json");
        send_json_line_fd(fd, resp);
        cJSON_Delete(resp);
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    const char *cmd_text = cJSON_IsString(cmd) ? cmd->valuestring : "help";
    cJSON *resp = cJSON_CreateObject();

    if (strcmp(cmd_text, "help") == 0) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON *arr = cJSON_AddArrayToObject(resp, "commands");
        cJSON_AddItemToArray(arr, cJSON_CreateString("help"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("list"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("get"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("get_all"));
        cJSON_AddItemToArray(arr, cJSON_CreateString("send"));
    } else if (strcmp(cmd_text, "list") == 0 || strcmp(cmd_text, "get_all") == 0) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON *units = cJSON_AddArrayToObject(resp, "units");
        if (xSemaphoreTake(s_telnet.app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
                d3net_unit_t *u = &s_telnet.app->gateway.units[i];
                if (!u->present) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddNumberToObject(o, "index", u->index);
                cJSON_AddStringToObject(o, "id", u->unit_id);
                cJSON_AddBoolToObject(o, "power", d3net_status_power_get(&u->status));
                cJSON_AddNumberToObject(o, "mode", d3net_status_oper_mode_get(&u->status));
                cJSON_AddNumberToObject(o, "setpoint", d3net_status_temp_setpoint_get(&u->status));
                cJSON_AddNumberToObject(o, "current_temp", d3net_status_temp_current_get(&u->status));
                cJSON_AddItemToArray(units, o);
            }
            xSemaphoreGive(s_telnet.app->gateway_lock);
        }
    } else if (strcmp(cmd_text, "get") == 0) {
        const cJSON *idx = cJSON_GetObjectItem(json, "index");
        const cJSON *id = cJSON_GetObjectItem(json, "id");
        d3net_unit_t *target = NULL;
        if (cJSON_IsNumber(idx)) {
            target = find_unit_by_index(idx->valueint);
        } else if (cJSON_IsString(id)) {
            target = find_unit_by_id(id->valuestring);
        }
        if (target == NULL) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "unit_not_found");
        } else {
            cJSON_AddBoolToObject(resp, "ok", true);
            cJSON_AddNumberToObject(resp, "index", target->index);
            cJSON_AddStringToObject(resp, "id", target->unit_id);
            cJSON_AddBoolToObject(resp, "power", d3net_status_power_get(&target->status));
            cJSON_AddNumberToObject(resp, "mode", d3net_status_oper_mode_get(&target->status));
            cJSON_AddNumberToObject(resp, "setpoint", d3net_status_temp_setpoint_get(&target->status));
            cJSON_AddNumberToObject(resp, "current_temp", d3net_status_temp_current_get(&target->status));
        }
    } else if (strcmp(cmd_text, "send") == 0) {
        const cJSON *idx = cJSON_GetObjectItem(json, "index");
        const cJSON *id = cJSON_GetObjectItem(json, "id");
        d3net_unit_t *target = NULL;
        if (cJSON_IsNumber(idx)) target = find_unit_by_index(idx->valueint);
        else if (cJSON_IsString(id)) target = find_unit_by_id(id->valuestring);
        if (target == NULL) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "unit_not_found");
        } else if (xSemaphoreTake(s_telnet.app->gateway_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_err_t err = ESP_OK;
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            const cJSON *power = cJSON_GetObjectItem(json, "power");
            const cJSON *mode = cJSON_GetObjectItem(json, "mode");
            const cJSON *temp = cJSON_GetObjectItem(json, "temp");
            const cJSON *fan = cJSON_GetObjectItem(json, "fan");
            const cJSON *fan_dir = cJSON_GetObjectItem(json, "fan_dir");
            const cJSON *filter_reset = cJSON_GetObjectItem(json, "filter_reset");
            if (cJSON_IsString(power)) {
                err = d3net_unit_set_power(&s_telnet.app->gateway, target, strcasecmp(power->valuestring, "on") == 0, now_ms);
            } else if (cJSON_IsBool(power)) {
                err = d3net_unit_set_power(&s_telnet.app->gateway, target, cJSON_IsTrue(power), now_ms);
            }
            if (err == ESP_OK && cJSON_IsString(mode)) err = d3net_unit_set_mode(&s_telnet.app->gateway, target, mode_from_text(mode->valuestring), now_ms);
            if (err == ESP_OK && cJSON_IsNumber(temp)) err = d3net_unit_set_setpoint(&s_telnet.app->gateway, target, (float)temp->valuedouble, now_ms);
            if (err == ESP_OK && cJSON_IsString(fan)) err = d3net_unit_set_fan_speed(&s_telnet.app->gateway, target, fan_from_text(fan->valuestring), now_ms);
            if (err == ESP_OK && cJSON_IsString(fan_dir)) err = d3net_unit_set_fan_dir(&s_telnet.app->gateway, target, fan_dir_from_text(fan_dir->valuestring), now_ms);
            if (err == ESP_OK && cJSON_IsTrue(filter_reset)) err = d3net_unit_filter_reset(&s_telnet.app->gateway, target, now_ms);
            xSemaphoreGive(s_telnet.app->gateway_lock);
            cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
            if (err != ESP_OK) cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        } else {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "gateway_busy");
        }
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "unknown_command");
    }

    send_json_line_fd(fd, resp);
    cJSON_Delete(resp);
    cJSON_Delete(json);
}

size_t telnet_server_get_logs(uint32_t since_seq, telnet_log_line_t *out, size_t max_lines) {
    size_t written = 0;
    if (s_telnet.log_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_telnet.log_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
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
    xSemaphoreGive(s_telnet.log_lock);
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
                        s_telnet.unit_power_valid[i] = false;
                        continue;
                    }
                    const bool power = d3net_status_power_get(&u->status);
                    telnet_server_logf(
                        "%s pwr=%d mode=%d set=%.1f cur=%.1f",
                        u->unit_id,
                        power,
                        d3net_status_oper_mode_get(&u->status),
                        d3net_status_temp_setpoint_get(&u->status),
                        d3net_status_temp_current_get(&u->status));
                    if (!s_telnet.unit_power_valid[i] || s_telnet.unit_power_cache[i] != power) {
                        s_telnet.unit_power_valid[i] = true;
                        s_telnet.unit_power_cache[i] = power;
                        sync_leds_for_unit_index_locked(i);
                    }
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
        s_telnet.client_buf_len[slot] = 0U;
        const char *hello = "D3Net telnet connected\r\n";
        send(sock, hello, strlen(hello), 0);
    }
}

static void process_dinplug_button_event(uint16_t keypad_id, uint16_t button_id, bool is_hold) {
    if (s_telnet.app == NULL || s_telnet.app->config.din_actions_json[0] == '\0') return;
    cJSON *maps = cJSON_Parse(s_telnet.app->config.din_actions_json);
    if (maps == NULL || !cJSON_IsArray(maps)) {
        cJSON_Delete(maps);
        return;
    }

    if (xSemaphoreTake(s_telnet.app->gateway_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cJSON_Delete(maps);
        return;
    }

    bool matched = false;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, maps) {
        if (!cJSON_IsObject(entry)) continue;
        cJSON *idx = cJSON_GetObjectItem(entry, "unit_index");
        if (!cJSON_IsNumber(idx)) continue;
        d3net_unit_t *u = find_unit_by_index(idx->valueint);
        if (u == NULL) continue;

        bool unit_has_keypad = false;
        cJSON *keypads = cJSON_GetObjectItem(entry, "keypads");
        if (cJSON_IsArray(keypads)) {
            cJSON *k = NULL;
            cJSON_ArrayForEach(k, keypads) {
                if (cJSON_IsNumber(k) && (uint16_t)k->valueint == keypad_id) {
                    unit_has_keypad = true;
                    break;
                }
            }
        }
        if (!unit_has_keypad) continue;

        cJSON *buttons = cJSON_GetObjectItem(entry, "buttons");
        if (!cJSON_IsArray(buttons)) continue;
        cJSON *b = NULL;
        cJSON_ArrayForEach(b, buttons) {
            if (!cJSON_IsObject(b)) continue;
            cJSON *kid = cJSON_GetObjectItem(b, "keypad_id");
            cJSON *bid = cJSON_GetObjectItem(b, "id");
            if (!cJSON_IsNumber(kid) || !cJSON_IsNumber(bid)) continue;
            if ((uint16_t)kid->valueint != keypad_id || (uint16_t)bid->valueint != button_id) continue;

            cJSON *act = cJSON_GetObjectItem(b, is_hold ? "hold_action" : "press_action");
            cJSON *val = cJSON_GetObjectItem(b, is_hold ? "hold_value" : "press_value");
            const char *action = cJSON_IsString(act) ? act->valuestring : "none";
            float value = cJSON_IsNumber(val) ? (float)val->valuedouble : 1.0f;
            if (value == 0.0f) value = 1.0f;
            esp_err_t err = apply_action_to_unit(u, action, value);
            matched = true;
            telnet_server_logf("dinplug button unit=%u keypad=%u btn=%u action=%s err=%s",
                               (unsigned)u->index,
                               (unsigned)keypad_id,
                               (unsigned)button_id,
                               action,
                               esp_err_to_name(err));
            if (err == ESP_OK) {
                s_telnet.unit_power_valid[u->index] = true;
                s_telnet.unit_power_cache[u->index] = d3net_status_power_get(&u->status);
                sync_leds_for_unit_entry(entry, u->index, s_telnet.unit_power_cache[u->index]);
            }
        }
    }

    xSemaphoreGive(s_telnet.app->gateway_lock);
    if (!matched) {
        telnet_server_logf("dinplug unmapped keypad=%u btn=%u hold=%u",
                           (unsigned)keypad_id,
                           (unsigned)button_id,
                           (unsigned)(is_hold ? 1 : 0));
    }
    cJSON_Delete(maps);
}

static void process_dinplug_line(const char *line) {
    if (line == NULL || line[0] == '\0') return;
    telnet_server_logf("dinplug rx: %s", line);
    if (strncmp(line, "R:BTN ", 6) != 0) return;

    char action[16] = {0};
    int keypad = 0;
    int button = 0;
    if (sscanf(line + 6, "%15s %d %d", action, &keypad, &button) != 3) return;
    bool is_hold = strcasecmp(action, "HOLD") == 0;
    bool is_press = strcasecmp(action, "PRESS") == 0;
    if (!is_hold && !is_press) return;
    if (keypad <= 0 || button <= 0) return;
    process_dinplug_button_event((uint16_t)keypad, (uint16_t)button, is_hold);
}

static void dinplug_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_telnet.app == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const bool auto_connect = s_telnet.app->config.din_auto_connect;
        const char *host = s_telnet.app->config.din_gateway_host;
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        const bool force_connect = s_telnet.dinplug_force_connect;
        if (!s_telnet.dinplug_connected && (auto_connect || force_connect) && host[0] != '\0' &&
            (force_connect || (now_ms - s_telnet.dinplug_last_attempt_ms >= DINPLUG_RECONNECT_MS))) {
            s_telnet.dinplug_force_connect = false;
            s_telnet.dinplug_last_attempt_ms = now_ms;
            int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (fd >= 0) {
                struct sockaddr_in addr = {
                    .sin_family = AF_INET,
                    .sin_port = htons(DINPLUG_PORT),
                };
                struct hostent *he = gethostbyname(host);
                if (he != NULL) {
                    memcpy(&addr.sin_addr.s_addr, he->h_addr_list[0], sizeof(addr.sin_addr.s_addr));
                    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                        s_telnet.dinplug_fd = fd;
                        s_telnet.dinplug_connected = true;
                        s_telnet.dinplug_buf_len = 0;
                        s_telnet.dinplug_last_keepalive_ms = now_ms;
                        telnet_server_logf("dinplug connected to %s:%u", host, DINPLUG_PORT);
                        if (xSemaphoreTake(s_telnet.app->gateway_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
                            sync_leds_for_all_units_locked();
                            xSemaphoreGive(s_telnet.app->gateway_lock);
                        }
                    } else {
                        close(fd);
                    }
                } else {
                    close(fd);
                }
            }
        }

        if (s_telnet.dinplug_connected) {
            if (now_ms - s_telnet.dinplug_last_keepalive_ms >= DINPLUG_KEEPALIVE_MS) {
                send(s_telnet.dinplug_fd, "STA\r\n", 5, 0);
                s_telnet.dinplug_last_keepalive_ms = now_ms;
            }
            char buf[128];
            int r = recv(s_telnet.dinplug_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r > 0) {
                for (int i = 0; i < r; i++) {
                    char ch = buf[i];
                    if (ch == '\r') continue;
                    if (ch == '\n') {
                        s_telnet.dinplug_buf[s_telnet.dinplug_buf_len] = '\0';
                        process_dinplug_line(s_telnet.dinplug_buf);
                        s_telnet.dinplug_buf_len = 0;
                    } else if (s_telnet.dinplug_buf_len + 1 < sizeof(s_telnet.dinplug_buf)) {
                        s_telnet.dinplug_buf[s_telnet.dinplug_buf_len++] = ch;
                    } else {
                        s_telnet.dinplug_buf_len = 0;
                    }
                }
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(s_telnet.dinplug_fd);
                s_telnet.dinplug_fd = -1;
                s_telnet.dinplug_connected = false;
                telnet_server_logf("dinplug disconnected");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void telnet_rx_task(void *arg) {
    (void)arg;
    while (true) {
        for (size_t i = 0; i < sizeof(s_telnet.clients) / sizeof(s_telnet.clients[0]); i++) {
            int fd = s_telnet.clients[i];
            if (fd < 0) continue;

            char buf[128];
            int r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r > 0) {
                for (int j = 0; j < r; j++) {
                    char ch = buf[j];
                    if (ch == '\r') continue;
                    if (ch == '\n') {
                        s_telnet.client_buf[i][s_telnet.client_buf_len[i]] = '\0';
                        if (s_telnet.client_buf_len[i] > 0) {
                            process_telnet_json(fd, s_telnet.client_buf[i]);
                        }
                        s_telnet.client_buf_len[i] = 0;
                    } else if (s_telnet.client_buf_len[i] + 1 < sizeof(s_telnet.client_buf[i])) {
                        s_telnet.client_buf[i][s_telnet.client_buf_len[i]++] = ch;
                    } else {
                        s_telnet.client_buf_len[i] = 0;
                    }
                }
            } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(fd);
                s_telnet.clients[i] = -1;
                s_telnet.client_buf_len[i] = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t telnet_server_start(app_context_t *app) {
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_telnet.app = app;
    s_telnet.listen_port = (app->config.telnet_port == 0U) ? 23U : app->config.telnet_port;
    if (s_telnet.log_lock == NULL) {
        s_telnet.log_lock = xSemaphoreCreateMutex();
    }
    xTaskCreate(telnet_accept_task, "telnet_accept", 4096, NULL, 5, NULL);
    xTaskCreate(telnet_rx_task, "telnet_rx", 6144, NULL, 5, NULL);
    xTaskCreate(telnet_status_task, "telnet_status", 4096, NULL, 4, NULL);
    xTaskCreate(dinplug_task, "dinplug", 6144, NULL, 4, NULL);
    return ESP_OK;
}
