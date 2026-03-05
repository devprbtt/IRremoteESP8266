#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telnet_server.h"
#include "wifi_manager.h"

#include "esp_spiffs.h"
static const char *TAG = "web_server";
static const char *DEFAULT_HOSTNAME = "daikin-d3net";
static const uint16_t DEFAULT_TELNET_PORT = 23;

static void copy_string(char *dst, size_t dst_len, const char *src) {
    if (dst == NULL || dst_len == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1U);
    dst[dst_len - 1U] = '\0';
}

static bool reg_is_set(const app_context_t *app, uint8_t index) {
    if (index >= D3NET_MAX_UNITS) {
        return false;
    }
    return (app->config.registered_mask & (1ULL << index)) != 0ULL;
}

static void reg_set(app_context_t *app, uint8_t index, bool on, const char *unit_id) {
    if (index >= D3NET_MAX_UNITS) {
        return;
    }
    if (on) {
        app->config.registered_mask |= (1ULL << index);
        if (unit_id != NULL) {
            copy_string(app->config.registered_ids[index], sizeof(app->config.registered_ids[index]), unit_id);
        }
    } else {
        app->config.registered_mask &= ~(1ULL << index);
        memset(app->config.registered_ids[index], 0, sizeof(app->config.registered_ids[index]));
    }
}

static const char *mode_name(d3net_mode_t mode) {
    switch (mode) {
    case D3NET_MODE_FAN:
        return "fan";
    case D3NET_MODE_HEAT:
        return "heat";
    case D3NET_MODE_COOL:
        return "cool";
    case D3NET_MODE_AUTO:
        return "auto";
    case D3NET_MODE_VENT:
        return "vent";
    case D3NET_MODE_UNDEFINED:
        return "undefined";
    case D3NET_MODE_SLAVE:
        return "slave";
    case D3NET_MODE_DRY:
        return "dry";
    default:
        return "unknown";
    }
}

static const char *WEB_FS_BASE = "/spiffs";
static bool s_web_fs_ready = false;

static esp_err_t mount_web_fs(void) {
    if (s_web_fs_ready) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = WEB_FS_BASE,
        .partition_label = "spiffs",
        .max_files = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes", (unsigned)used, (unsigned)total);
    }

    s_web_fs_ready = true;
    return ESP_OK;
}

static esp_err_t send_file_page(httpd_req_t *req, const char *path) {
    if (!s_web_fs_ready) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "web fs not mounted");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "page not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    char chunk[1024];
    while (true) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t http_reply_json(httpd_req_t *req, cJSON *root) {
    char *body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    cJSON_Delete(root);
    return err;
}

static int recv_body(httpd_req_t *req, char **out, size_t *out_len) {
    if (req->content_len <= 0) {
        *out = NULL;
        *out_len = 0;
        return ESP_OK;
    }
    char *buf = calloc(1, (size_t)req->content_len + 1U);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        total += r;
    }
    *out = buf;
    *out_len = (size_t)total;
    return ESP_OK;
}

static bool await_sta_ip(char *ip_out, size_t ip_out_len, uint32_t timeout_ms) {
    if (ip_out != NULL && ip_out_len > 0U) {
        ip_out[0] = '\0';
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() <= deadline) {
        char ip[32] = {0};
        if (wifi_manager_sta_connected() && wifi_manager_sta_ip(ip, sizeof(ip)) == ESP_OK && strcmp(ip, "0.0.0.0") != 0) {
            if (ip_out != NULL && ip_out_len > 0U) {
                copy_string(ip_out, ip_out_len, ip);
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (ip_out != NULL && ip_out_len > 0U) {
        char ip[32] = {0};
        if (wifi_manager_sta_ip(ip, sizeof(ip)) == ESP_OK && strcmp(ip, "0.0.0.0") != 0) {
            copy_string(ip_out, ip_out_len, ip);
        }
    }
    return false;
}

static esp_err_t handle_index_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/index.html");
}

static esp_err_t handle_d3net_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/d3net.html");
}

static esp_err_t handle_config_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/config.html");
}

static esp_err_t handle_rtu_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/rtu.html");
}

static esp_err_t handle_dinplug_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/dinplug.html");
}

static esp_err_t handle_monitor_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/monitor.html");
}

static esp_err_t handle_firmware_page_get(httpd_req_t *req) {
    return send_file_page(req, "/spiffs/firmware.html");
}

static esp_err_t handle_status_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char ip[32] = {0};
    wifi_manager_sta_ip(ip, sizeof(ip));

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", wifi_manager_sta_connected());
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(root, "hostname", app->config.hostname[0] ? app->config.hostname : DEFAULT_HOSTNAME);
    cJSON_AddNumberToObject(root, "telnet_port", app->config.telnet_port ? app->config.telnet_port : DEFAULT_TELNET_PORT);

    cJSON *ota = cJSON_CreateObject();
    cJSON_AddBoolToObject(ota, "active", app->ota.active);
    cJSON_AddBoolToObject(ota, "success", app->ota.success);
    cJSON_AddNumberToObject(ota, "bytes_received", (double)app->ota.bytes_received);
    cJSON_AddNumberToObject(ota, "total_bytes", (double)app->ota.total_bytes);
    cJSON_AddStringToObject(ota, "message", app->ota.message);
    cJSON_AddItemToObject(root, "ota", ota);

    return http_reply_json(req, root);
}

static esp_err_t handle_wifi_scan_get(httpd_req_t *req) {
    wifi_scan_item_t items[20];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(items, 20, &count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", items[i].rssi);
        cJSON_AddNumberToObject(item, "auth", items[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "items", arr);
    cJSON *arr2 = cJSON_Duplicate(arr, 1);
    if (arr2 != NULL) {
        cJSON_AddItemToObject(root, "networks", arr2);
    }
    return http_reply_json(req, root);
}

static esp_err_t handle_wifi_connect_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring == NULL) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_manager_set_sta_network(
        app->config.sta_dhcp, app->config.sta_ip, app->config.sta_gateway, app->config.sta_subnet, app->config.sta_dns);
    if (err == ESP_OK) {
        err = wifi_manager_connect_sta(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    }
    if (err == ESP_OK) {
        copy_string(app->config.sta_ssid, sizeof(app->config.sta_ssid), ssid->valuestring);
        if (cJSON_IsString(pass) && pass->valuestring != NULL) {
            copy_string(app->config.sta_password, sizeof(app->config.sta_password), pass->valuestring);
        } else {
            app->config.sta_password[0] = '\0';
        }
        app->config.sta_configured = true;
        config_store_save(&app->config);
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connect failed");
        return err;
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_api_config_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", app->config.sta_ssid);
    cJSON_AddStringToObject(wifi, "password", app->config.sta_password);
    cJSON_AddBoolToObject(wifi, "dhcp", app->config.sta_dhcp);
    cJSON_AddStringToObject(wifi, "ip", app->config.sta_ip);
    cJSON_AddStringToObject(wifi, "gateway", app->config.sta_gateway);
    cJSON_AddStringToObject(wifi, "subnet", app->config.sta_subnet);
    cJSON_AddStringToObject(wifi, "dns", app->config.sta_dns);
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddStringToObject(root, "hostname", app->config.hostname[0] ? app->config.hostname : DEFAULT_HOSTNAME);
    cJSON_AddNumberToObject(root, "telnet_port", app->config.telnet_port ? app->config.telnet_port : DEFAULT_TELNET_PORT);
    cJSON *dinplug = cJSON_CreateObject();
    cJSON_AddStringToObject(dinplug, "gateway_host", app->config.din_gateway_host);
    cJSON_AddBoolToObject(dinplug, "auto_connect", app->config.din_auto_connect);
    cJSON_AddItemToObject(root, "dinplug", dinplug);
    return http_reply_json(req, root);
}

static esp_err_t handle_api_config_save_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "password");
    cJSON *dhcp = cJSON_GetObjectItem(json, "dhcp");
    cJSON *ip = cJSON_GetObjectItem(json, "ip");
    cJSON *gateway = cJSON_GetObjectItem(json, "gateway");
    cJSON *subnet = cJSON_GetObjectItem(json, "subnet");
    cJSON *dns = cJSON_GetObjectItem(json, "dns");
    cJSON *hostname = cJSON_GetObjectItem(json, "hostname");
    cJSON *telnet_port = cJSON_GetObjectItem(json, "telnet_port");

    if (cJSON_IsString(ssid) && ssid->valuestring != NULL) {
        copy_string(app->config.sta_ssid, sizeof(app->config.sta_ssid), ssid->valuestring);
    }
    if (cJSON_IsString(pass) && pass->valuestring != NULL) {
        copy_string(app->config.sta_password, sizeof(app->config.sta_password), pass->valuestring);
    }
    app->config.sta_configured = app->config.sta_ssid[0] != '\0';
    if (cJSON_IsTrue(dhcp) || cJSON_IsFalse(dhcp)) {
        app->config.sta_dhcp = cJSON_IsTrue(dhcp);
    }
    if (cJSON_IsString(ip) && ip->valuestring != NULL) {
        copy_string(app->config.sta_ip, sizeof(app->config.sta_ip), ip->valuestring);
    }
    if (cJSON_IsString(gateway) && gateway->valuestring != NULL) {
        copy_string(app->config.sta_gateway, sizeof(app->config.sta_gateway), gateway->valuestring);
    }
    if (cJSON_IsString(subnet) && subnet->valuestring != NULL) {
        copy_string(app->config.sta_subnet, sizeof(app->config.sta_subnet), subnet->valuestring);
    }
    if (cJSON_IsString(dns) && dns->valuestring != NULL) {
        copy_string(app->config.sta_dns, sizeof(app->config.sta_dns), dns->valuestring);
    }
    if (cJSON_IsString(hostname) && hostname->valuestring != NULL && hostname->valuestring[0] != '\0') {
        copy_string(app->config.hostname, sizeof(app->config.hostname), hostname->valuestring);
    } else if (app->config.hostname[0] == '\0') {
        copy_string(app->config.hostname, sizeof(app->config.hostname), DEFAULT_HOSTNAME);
    }
    if (cJSON_IsNumber(telnet_port) && telnet_port->valueint > 0 && telnet_port->valueint <= 65535) {
        app->config.telnet_port = (uint16_t)telnet_port->valueint;
    } else if (app->config.telnet_port == 0U) {
        app->config.telnet_port = DEFAULT_TELNET_PORT;
    }
    cJSON_Delete(json);

    esp_err_t err = wifi_manager_set_sta_network(
        app->config.sta_dhcp, app->config.sta_ip, app->config.sta_gateway, app->config.sta_subnet, app->config.sta_dns);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ip config");
        return err;
    }
    bool sta_connected = false;
    char sta_ip[32] = {0};
    if (app->config.sta_configured) {
        err = wifi_manager_connect_sta(app->config.sta_ssid, app->config.sta_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA connect failed after save: %s", esp_err_to_name(err));
        } else {
            sta_connected = await_sta_ip(sta_ip, sizeof(sta_ip), 8000U);
        }
    }
    config_store_save(&app->config);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "reboot", true);
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    http_reply_json(req, root);

    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_dinplug_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_host", app->config.din_gateway_host);
    cJSON_AddBoolToObject(root, "auto_connect", app->config.din_auto_connect);
    cJSON_AddBoolToObject(root, "connected", telnet_server_dinplug_connected());
    cJSON_AddStringToObject(root, "actions_json", app->config.din_actions_json[0] ? app->config.din_actions_json : "[]");
    return http_reply_json(req, root);
}

static esp_err_t handle_dinplug_save_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *host = cJSON_GetObjectItem(json, "gateway_host");
    cJSON *auto_connect = cJSON_GetObjectItem(json, "auto_connect");
    cJSON *actions_json = cJSON_GetObjectItem(json, "actions_json");
    if (cJSON_IsString(host) && host->valuestring != NULL) {
        copy_string(app->config.din_gateway_host, sizeof(app->config.din_gateway_host), host->valuestring);
    }
    if (cJSON_IsTrue(auto_connect) || cJSON_IsFalse(auto_connect)) {
        app->config.din_auto_connect = cJSON_IsTrue(auto_connect);
    }
    if (cJSON_IsString(actions_json) && actions_json->valuestring != NULL) {
        copy_string(app->config.din_actions_json, sizeof(app->config.din_actions_json), actions_json->valuestring);
    }
    cJSON_Delete(json);
    config_store_save(&app->config);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_dinplug_connect_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    if (req->content_len > 0) {
        char *body = NULL;
        size_t body_len = 0;
        if (recv_body(req, &body, &body_len) == ESP_OK && body != NULL) {
            cJSON *json = cJSON_ParseWithLength(body, body_len);
            free(body);
            if (json != NULL) {
                cJSON *host = cJSON_GetObjectItem(json, "gateway_host");
                if (cJSON_IsString(host) && host->valuestring != NULL) {
                    copy_string(app->config.din_gateway_host, sizeof(app->config.din_gateway_host), host->valuestring);
                    config_store_save(&app->config);
                }
                cJSON_Delete(json);
            }
        }
    }
    if (app->config.din_gateway_host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gateway host not configured");
        return ESP_ERR_INVALID_STATE;
    }
    telnet_server_request_dinplug_connect();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_hvac_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
            d3net_unit_t *u = &app->gateway.units[i];
            if (!u->present) {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", u->index);
            cJSON_AddStringToObject(item, "unit_id", u->unit_id);
            cJSON_AddBoolToObject(item, "power", d3net_status_power_get(&u->status));
            cJSON_AddNumberToObject(item, "mode", d3net_status_oper_mode_get(&u->status));
            cJSON_AddNumberToObject(item, "temp_current", d3net_status_temp_current_get(&u->status));
            cJSON_AddNumberToObject(item, "temp_setpoint", d3net_status_temp_setpoint_get(&u->status));
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(app->gateway_lock);
    }

    cJSON_AddItemToObject(root, "units", arr);
    return http_reply_json(req, root);
}

static esp_err_t handle_discover_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    esp_err_t err = ESP_FAIL;
    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        err = d3net_gateway_discover_units(&app->gateway);
        xSemaphoreGive(app->gateway_lock);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "discover failed");
        return err;
    }
    telnet_server_logf("discovery complete: units=%u", app->gateway.discovered_count);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_hvac_cmd_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    cJSON *idx = cJSON_GetObjectItem(json, "index");
    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    cJSON *val = cJSON_GetObjectItem(json, "value");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(cmd)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fields");
        return ESP_FAIL;
    }
    int index = idx->valueint;
    if (index < 0 || index >= D3NET_MAX_UNITS) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    const char *cmd_text = cmd->valuestring;
    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(5000)) == pdTRUE) {
        d3net_unit_t *u = &app->gateway.units[index];
        if (!u->present) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            if (strcmp(cmd->valuestring, "power") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_power(&app->gateway, u, val->valuedouble > 0.5, now_ms);
            } else if (strcmp(cmd->valuestring, "mode") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_mode(&app->gateway, u, (d3net_mode_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "setpoint") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_setpoint(&app->gateway, u, (float)val->valuedouble, now_ms);
            } else if (strcmp(cmd->valuestring, "fan_speed") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_fan_speed(&app->gateway, u, (d3net_fan_speed_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "fan_dir") == 0 && cJSON_IsNumber(val)) {
                err = d3net_unit_set_fan_dir(&app->gateway, u, (d3net_fan_dir_t)val->valueint, now_ms);
            } else if (strcmp(cmd->valuestring, "filter_reset") == 0) {
                err = d3net_unit_filter_reset(&app->gateway, u, now_ms);
            } else {
                err = ESP_ERR_INVALID_ARG;
            }
        }
        xSemaphoreGive(app->gateway_lock);
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "command failed");
        return err;
    }
    telnet_server_logf("hvac cmd idx=%d %s", index, cmd_text);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_registry_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    bool present_map[D3NET_MAX_UNITS] = {0};

    if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
            d3net_unit_t *u = &app->gateway.units[i];
            if (!u->present) {
                continue;
            }
            present_map[i] = true;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "index", u->index);
            cJSON_AddStringToObject(item, "unit_id", u->unit_id);
            cJSON_AddBoolToObject(item, "registered", reg_is_set(app, i));
            cJSON_AddBoolToObject(item, "online", true);
            cJSON_AddBoolToObject(item, "power", d3net_status_power_get(&u->status));
            int mode = d3net_status_oper_mode_get(&u->status);
            cJSON_AddNumberToObject(item, "mode", mode);
            cJSON_AddStringToObject(item, "mode_name", mode_name((d3net_mode_t)mode));

            float temp_current = d3net_status_temp_current_get(&u->status);
            temp_current = roundf(temp_current * 100.0f) / 100.0f;
            cJSON_AddNumberToObject(item, "temp_current", temp_current);
            cJSON_AddNumberToObject(item, "temp_setpoint", d3net_status_temp_setpoint_get(&u->status));
            cJSON_AddItemToArray(arr, item);
        }
        xSemaphoreGive(app->gateway_lock);
    }

    for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
        if (!reg_is_set(app, i)) {
            continue;
        }
        if (present_map[i]) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "unit_id", app->config.registered_ids[i]);
        cJSON_AddBoolToObject(item, "registered", true);
        cJSON_AddBoolToObject(item, "online", false);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "units", arr);
    return http_reply_json(req, root);
}

static esp_err_t handle_registry_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *idx = cJSON_GetObjectItem(json, "index");
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fields");
        return ESP_FAIL;
    }
    int index = idx->valueint;
    if (index < 0 || index >= D3NET_MAX_UNITS) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(action->valuestring, "add") == 0) {
        if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
            d3net_unit_t *u = &app->gateway.units[index];
            if (!u->present) {
                err = ESP_ERR_NOT_FOUND;
            } else {
                reg_set(app, index, true, u->unit_id);
                config_store_save(&app->config);
                telnet_server_logf("registered unit idx=%d id=%s", index, u->unit_id);
            }
            xSemaphoreGive(app->gateway_lock);
        }
    } else if (strcmp(action->valuestring, "remove") == 0) {
        reg_set(app, index, false, NULL);
        config_store_save(&app->config);
        telnet_server_logf("unregistered unit idx=%d", index);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "registry");
        return err;
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_logs_get(httpd_req_t *req) {
    char query[64] = {0};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char buf[16] = {0};
        if (httpd_query_key_value(query, "since", buf, sizeof(buf)) == ESP_OK) {
            since = (uint32_t)strtoul(buf, NULL, 10);
        }
    }
    const size_t max_lines = 32;
    telnet_log_line_t *lines = calloc(max_lines, sizeof(telnet_log_line_t));
    if (lines == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_ERR_NO_MEM;
    }
    size_t count = telnet_server_get_logs(since, lines, max_lines);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    uint32_t latest = since;
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", lines[i].seq);
        cJSON_AddStringToObject(item, "text", lines[i].line);
        cJSON_AddItemToArray(arr, item);
        if (lines[i].seq > latest) {
            latest = lines[i].seq;
        }
    }
    cJSON_AddItemToObject(root, "lines", arr);
    cJSON_AddNumberToObject(root, "latest", latest);
    cJSON_AddBoolToObject(root, "enabled", telnet_server_monitor_enabled());
    free(lines);
    return http_reply_json(req, root);
}

static esp_err_t handle_monitor_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", telnet_server_monitor_enabled());
    return http_reply_json(req, root);
}

static esp_err_t handle_monitor_toggle_post(httpd_req_t *req) {
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (!cJSON_IsTrue(enabled) && !cJSON_IsFalse(enabled)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled");
        return ESP_FAIL;
    }
    telnet_server_set_monitor_enabled(cJSON_IsTrue(enabled));
    cJSON_Delete(json);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_monitor_clear_post(httpd_req_t *req) {
    telnet_server_clear_logs();
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_rtu_get(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    const modbus_rtu_config_t *cfg = &app->config.rtu_cfg;
    cJSON_AddNumberToObject(root, "tx_pin", cfg->tx_pin);
    cJSON_AddNumberToObject(root, "rx_pin", cfg->rx_pin);
    cJSON_AddNumberToObject(root, "de_pin", cfg->de_pin);
    cJSON_AddNumberToObject(root, "re_pin", cfg->re_pin);
    cJSON_AddNumberToObject(root, "baud_rate", cfg->baud_rate);
    cJSON_AddNumberToObject(root, "data_bits", cfg->data_bits);
    cJSON_AddNumberToObject(root, "stop_bits", cfg->stop_bits);
    cJSON_AddStringToObject(root, "parity", (char[]){cfg->parity, 0});
    cJSON_AddNumberToObject(root, "slave_id", cfg->slave_id);
    cJSON_AddNumberToObject(root, "timeout_ms", cfg->timeout_ms);
    cJSON *defaults = cJSON_CreateObject();
    cJSON_AddNumberToObject(defaults, "tx_pin", 17);
    cJSON_AddNumberToObject(defaults, "rx_pin", 16);
    cJSON_AddNumberToObject(defaults, "de_pin", 4);
    cJSON_AddNumberToObject(defaults, "re_pin", 5);
    cJSON_AddNumberToObject(defaults, "baud_rate", 19200);
    cJSON_AddNumberToObject(defaults, "data_bits", 8);
    cJSON_AddNumberToObject(defaults, "stop_bits", 2);
    cJSON_AddStringToObject(defaults, "parity", "N");
    cJSON_AddNumberToObject(defaults, "slave_id", 1);
    cJSON_AddNumberToObject(defaults, "timeout_ms", 3000);
    cJSON_AddItemToObject(root, "defaults", defaults);
    return http_reply_json(req, root);
}

static esp_err_t handle_rtu_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    char *body = NULL;
    size_t body_len = 0;
    if (recv_body(req, &body, &body_len) != ESP_OK || body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *json = cJSON_ParseWithLength(body, body_len);
    free(body);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    modbus_rtu_config_t *cfg = &app->config.rtu_cfg;
    cfg->tx_pin = cJSON_GetObjectItem(json, "tx_pin")->valueint;
    cfg->rx_pin = cJSON_GetObjectItem(json, "rx_pin")->valueint;
    cfg->de_pin = cJSON_GetObjectItem(json, "de_pin")->valueint;
    cfg->re_pin = cJSON_GetObjectItem(json, "re_pin")->valueint;
    cfg->baud_rate = cJSON_GetObjectItem(json, "baud_rate")->valueint;
    cfg->data_bits = cJSON_GetObjectItem(json, "data_bits")->valueint;
    cfg->stop_bits = cJSON_GetObjectItem(json, "stop_bits")->valueint;
    cfg->parity = (char)cJSON_GetObjectItem(json, "parity")->valuestring[0];
    cfg->slave_id = cJSON_GetObjectItem(json, "slave_id")->valueint;
    cfg->timeout_ms = cJSON_GetObjectItem(json, "timeout_ms")->valueint;
    cJSON_Delete(json);

    config_store_save(&app->config);
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_ota_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    app->ota.active = true;
    app->ota.success = false;
    app->ota.bytes_received = 0;
    app->ota.total_bytes = (size_t)req->content_len;
    strncpy(app->ota.message, "OTA receiving", sizeof(app->ota.message) - 1U);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        strncpy(app->ota.message, "No OTA partition", sizeof(app->ota.message) - 1U);
        app->ota.active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        strncpy(app->ota.message, "OTA begin failed", sizeof(app->ota.message) - 1U);
        app->ota.active = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
        return err;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            esp_ota_abort(ota_handle);
            app->ota.active = false;
            strncpy(app->ota.message, "OTA read failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota read");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            app->ota.active = false;
            strncpy(app->ota.message, "OTA write failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write");
            return err;
        }
        remaining -= r;
        app->ota.bytes_received += (size_t)r;
    }

    err = esp_ota_end(ota_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }
    app->ota.active = false;
    app->ota.success = (err == ESP_OK);
    strncpy(app->ota.message, err == ESP_OK ? "OTA complete, rebooting" : "OTA finalize failed", sizeof(app->ota.message) - 1U);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end");
        return err;
    }

    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_spiffs_update_post(httpd_req_t *req) {
    app_context_t *app = (app_context_t *)req->user_ctx;
    app->ota.active = true;
    app->ota.success = false;
    app->ota.bytes_received = 0;
    app->ota.total_bytes = (size_t)req->content_len;
    strncpy(app->ota.message, "SPIFFS update receiving", sizeof(app->ota.message) - 1U);

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (part == NULL) {
        app->ota.active = false;
        strncpy(app->ota.message, "SPIFFS partition not found", sizeof(app->ota.message) - 1U);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs partition");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || (size_t)req->content_len > part->size) {
        app->ota.active = false;
        strncpy(app->ota.message, "Invalid SPIFFS image size", sizeof(app->ota.message) - 1U);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image size");
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_web_fs_ready) {
        esp_vfs_spiffs_unregister("spiffs");
        s_web_fs_ready = false;
    }

    const size_t erase_size = ((size_t)req->content_len + 4095U) & ~4095U;
    esp_err_t err = esp_partition_erase_range(part, 0, erase_size);
    if (err != ESP_OK) {
        app->ota.active = false;
        strncpy(app->ota.message, "SPIFFS erase failed", sizeof(app->ota.message) - 1U);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs erase");
        return err;
    }

    char buf[1024];
    size_t offset = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            app->ota.active = false;
            strncpy(app->ota.message, "SPIFFS read failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs read");
            return ESP_FAIL;
        }
        err = esp_partition_write(part, offset, buf, (size_t)r);
        if (err != ESP_OK) {
            app->ota.active = false;
            strncpy(app->ota.message, "SPIFFS write failed", sizeof(app->ota.message) - 1U);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs write");
            return err;
        }
        remaining -= r;
        offset += (size_t)r;
        app->ota.bytes_received = offset;
    }

    app->ota.active = false;
    app->ota.success = true;
    strncpy(app->ota.message, "SPIFFS update complete, rebooting", sizeof(app->ota.message) - 1U);
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_server_start(app_context_t *app, httpd_handle_t *out_handle) {
    if (app == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mount_web_fs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "web pages unavailable until SPIFFS is uploaded");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index_get, .user_ctx = app},
        {.uri = "/d3net", .method = HTTP_GET, .handler = handle_d3net_page_get, .user_ctx = app},
        {.uri = "/config", .method = HTTP_GET, .handler = handle_config_page_get, .user_ctx = app},
        {.uri = "/rtu", .method = HTTP_GET, .handler = handle_rtu_page_get, .user_ctx = app},
        {.uri = "/dinplug", .method = HTTP_GET, .handler = handle_dinplug_page_get, .user_ctx = app},
        {.uri = "/monitor", .method = HTTP_GET, .handler = handle_monitor_page_get, .user_ctx = app},
        {.uri = "/firmware", .method = HTTP_GET, .handler = handle_firmware_page_get, .user_ctx = app},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status_get, .user_ctx = app},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_api_config_get, .user_ctx = app},
        {.uri = "/api/config/save", .method = HTTP_POST, .handler = handle_api_config_save_post, .user_ctx = app},
        {.uri = "/api/dinplug", .method = HTTP_GET, .handler = handle_dinplug_get, .user_ctx = app},
        {.uri = "/api/dinplug/save", .method = HTTP_POST, .handler = handle_dinplug_save_post, .user_ctx = app},
        {.uri = "/api/dinplug/connect", .method = HTTP_POST, .handler = handle_dinplug_connect_post, .user_ctx = app},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan_get, .user_ctx = app},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect_post, .user_ctx = app},
        {.uri = "/api/hvac", .method = HTTP_GET, .handler = handle_hvac_get, .user_ctx = app},
        {.uri = "/api/discover", .method = HTTP_POST, .handler = handle_discover_post, .user_ctx = app},
        {.uri = "/api/hvac/cmd", .method = HTTP_POST, .handler = handle_hvac_cmd_post, .user_ctx = app},
        {.uri = "/api/registry", .method = HTTP_GET, .handler = handle_registry_get, .user_ctx = app},
        {.uri = "/api/registry", .method = HTTP_POST, .handler = handle_registry_post, .user_ctx = app},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs_get, .user_ctx = app},
        {.uri = "/api/monitor", .method = HTTP_GET, .handler = handle_monitor_get, .user_ctx = app},
        {.uri = "/api/monitor/toggle", .method = HTTP_POST, .handler = handle_monitor_toggle_post, .user_ctx = app},
        {.uri = "/api/monitor/clear", .method = HTTP_POST, .handler = handle_monitor_clear_post, .user_ctx = app},
        {.uri = "/api/rtu", .method = HTTP_GET, .handler = handle_rtu_get, .user_ctx = app},
        {.uri = "/api/rtu", .method = HTTP_POST, .handler = handle_rtu_post, .user_ctx = app},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota_post, .user_ctx = app},
        {.uri = "/firmware/update", .method = HTTP_POST, .handler = handle_ota_post, .user_ctx = app},
        {.uri = "/api/spiffs/update", .method = HTTP_POST, .handler = handle_spiffs_update_post, .user_ctx = app},
        {.uri = "/spiffs/update", .method = HTTP_POST, .handler = handle_spiffs_update_post, .user_ctx = app},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "handler register failed for %s", routes[i].uri);
        }
    }

    *out_handle = server;
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
