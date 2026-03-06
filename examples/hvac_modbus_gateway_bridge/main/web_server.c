#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_server";
static const char *WEB_FS_BASE = "/spiffs";
static bool s_web_fs_ready = false;

struct web_server {
    app_config_t *cfg;
    hvac_manager_t *hvac;
    wifi_manager_t *wifi;
    httpd_handle_t http;
};

static esp_err_t mount_web_fs(void)
{
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
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
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

static esp_err_t send_file_page(httpd_req_t *req, const char *path)
{
    if (!s_web_fs_ready) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "web fs not mounted");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "page not found");
        return ESP_FAIL;
    }

    if (strstr(path, ".css") != NULL) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(path, ".js") != NULL) {
        httpd_resp_set_type(req, "application/javascript");
    } else {
        httpd_resp_set_type(req, "text/html");
    }

    char chunk[1024];
    while (1) {
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

static esp_err_t recv_body(httpd_req_t *req, char **out)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += r;
    }

    *out = buf;
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}

static esp_err_t handle_home(httpd_req_t *req)
{
    return send_file_page(req, "/spiffs/index.html");
}

static esp_err_t handle_config_page(httpd_req_t *req)
{
    return send_file_page(req, "/spiffs/config.html");
}

static esp_err_t handle_monitor_page(httpd_req_t *req)
{
    return send_file_page(req, "/spiffs/monitor.html");
}

static esp_err_t handle_status(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char ip[32] = "0.0.0.0";
    char ap_ssid[33] = {0};
    wifi_manager_get_ip(s->wifi, ip, sizeof(ip));
    wifi_manager_get_ap_ssid(s->wifi, ap_ssid, sizeof(ap_ssid));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", s->cfg->system.device_id);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected(s->wifi));
    cJSON_AddStringToObject(root, "wifi_mode", wifi_manager_is_ap_mode(s->wifi) ? "ap" : "sta");
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "idu_address_base", s->cfg->hvac.idu_address_base);
    cJSON_AddNumberToObject(root, "poll_interval_ms", s->cfg->hvac.poll_interval_ms);
    return send_json(req, root);
}

static esp_err_t handle_zones(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char zones[4096] = {0};
    if (hvac_manager_zones_to_json(s->hvac, zones, sizeof(zones)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "zones encode failed");
    }

    char out[4608];
    snprintf(out, sizeof(out), "{\"ok\":true,\"zones\":%s}", zones);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char json[3072] = {0};
    if (app_config_to_json(s->cfg, json, sizeof(json)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config encode failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }

    uint16_t count = 20;
    wifi_ap_record_t recs[20] = {0};
    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan records failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(item, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "items", arr);
    return send_json(req, root);
}

static void apply_optional_int(cJSON *json, const char *key, int *dest)
{
    cJSON *n = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(n)) {
        *dest = n->valueint;
    }
}

static esp_err_t handle_config_save(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (cJSON_IsString(ssid) && ssid->valuestring) {
        app_config_set_wifi(s->cfg, ssid->valuestring,
                            (cJSON_IsString(password) && password->valuestring) ? password->valuestring : "");
    }

    cJSON *zones = cJSON_GetObjectItemCaseSensitive(json, "zones");
    if (cJSON_IsString(zones) && zones->valuestring && zones->valuestring[0] != '\0') {
        if (app_config_set_zones_from_csv(s->cfg, zones->valuestring) != ESP_OK) {
            cJSON_Delete(json);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid zones csv");
        }
    }

    apply_optional_int(json, "idu_address_base", &s->cfg->hvac.idu_address_base);
    apply_optional_int(json, "poll_interval_ms", &s->cfg->hvac.poll_interval_ms);
    cJSON *gateway_type = cJSON_GetObjectItemCaseSensitive(json, "gateway_type");
    if (cJSON_IsString(gateway_type) && gateway_type->valuestring) {
        if (strcmp(gateway_type->valuestring, "midea_gw3_mod") == 0 || strcmp(gateway_type->valuestring, "midea") == 0) {
            s->cfg->hvac.gateway_type = HVAC_GATEWAY_MIDEA_GW3_MOD;
            if (s->cfg->hvac.idu_address_base != 0) {
                s->cfg->hvac.idu_address_base = 0;
            }
        } else if (strcmp(gateway_type->valuestring, "daikin_dta116a51") == 0 || strcmp(gateway_type->valuestring, "daikin") == 0) {
            s->cfg->hvac.gateway_type = HVAC_GATEWAY_DAIKIN_DTA116A51;
            if (s->cfg->hvac.idu_address_base != 0) {
                s->cfg->hvac.idu_address_base = 0;
            }
        } else {
            s->cfg->hvac.gateway_type = HVAC_GATEWAY_LG_PMBUSB00A;
        }
    }

    cJSON *default_slave = cJSON_GetObjectItemCaseSensitive(json, "default_slave_id");
    if (cJSON_IsNumber(default_slave)) {
        s->cfg->modbus.default_slave_id = (uint8_t)default_slave->valueint;
    }

    apply_optional_int(json, "tx_pin", &s->cfg->modbus.tx_pin);
    apply_optional_int(json, "rx_pin", &s->cfg->modbus.rx_pin);
    apply_optional_int(json, "de_pin", &s->cfg->modbus.de_pin);
    apply_optional_int(json, "baud", &s->cfg->modbus.baud);
    apply_optional_int(json, "timeout_ms", &s->cfg->modbus.timeout_ms);
    apply_optional_int(json, "retries", &s->cfg->modbus.retries);
    apply_optional_int(json, "setpoint_min_tenths", &s->cfg->hvac.setpoint_min_tenths);
    apply_optional_int(json, "setpoint_max_tenths", &s->cfg->hvac.setpoint_max_tenths);
    apply_optional_int(json, "mode_rate_limit_ms", &s->cfg->hvac.mode_rate_limit_ms);
    apply_optional_int(json, "log_level", &s->cfg->system.log_level);

    cJSON *device_id = cJSON_GetObjectItemCaseSensitive(json, "device_id");
    if (cJSON_IsString(device_id) && device_id->valuestring && device_id->valuestring[0]) {
        strlcpy(s->cfg->system.device_id, device_id->valuestring, sizeof(s->cfg->system.device_id));
    }

    cJSON_Delete(json);

    if (s->cfg->hvac.idu_address_base != 0 && s->cfg->hvac.idu_address_base != 1) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "idu_address_base must be 0 or 1");
    }
    if (s->cfg->modbus.default_slave_id < 1 || s->cfg->modbus.default_slave_id > 247) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "default_slave_id must be 1..247");
    }

    if (app_config_save(s->cfg) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddBoolToObject(ok, "reboot", true);
    cJSON_AddStringToObject(ok, "msg", "saved to NVS; rebooting");
    send_json(req, ok);

    xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 4, NULL);
    return ESP_OK;
}

static esp_err_t handle_hvac_cmd(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    cJSON *idx = cJSON_GetObjectItemCaseSensitive(json, "index");
    cJSON *field = cJSON_GetObjectItemCaseSensitive(json, "field");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
    if (!cJSON_IsNumber(idx) || !cJSON_IsString(field) || (!cJSON_IsString(value) && !cJSON_IsNumber(value))) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index/field/value required");
    }

    char value_buf[24] = {0};
    const char *value_str = NULL;
    if (cJSON_IsString(value)) {
        value_str = value->valuestring;
    } else {
        snprintf(value_buf, sizeof(value_buf), "%g", value->valuedouble);
        value_str = value_buf;
    }

    char msg[96];
    esp_err_t err = hvac_manager_set_field(s->hvac, (size_t)idx->valueint, field->valuestring, value_str, msg, sizeof(msg));
    cJSON_Delete(json);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(out, "err", err);
    cJSON_AddStringToObject(out, "msg", msg);
    return send_json(req, out);
}

static esp_err_t handle_hvac_read(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    cJSON *idx = cJSON_GetObjectItemCaseSensitive(json, "index");
    if (!cJSON_IsNumber(idx)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index required");
    }
    esp_err_t err = hvac_manager_refresh_zone(s->hvac, (size_t)idx->valueint);
    cJSON_Delete(json);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(out, "err", err);
    return send_json(req, out);
}

esp_err_t web_server_start(web_server_t **out_server, const web_server_config_t *cfg)
{
    if (!out_server || !cfg || !cfg->cfg || !cfg->hvac || !cfg->wifi) {
        return ESP_ERR_INVALID_ARG;
    }

    web_server_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->cfg = cfg->cfg;
    s->hvac = cfg->hvac;
    s->wifi = cfg->wifi;

    esp_err_t err = mount_web_fs();
    if (err != ESP_OK) {
        free(s);
        return err;
    }

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 24;
    hcfg.stack_size = 8192;

    err = httpd_start(&s->http, &hcfg);
    if (err != ESP_OK) {
        free(s);
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_home, .user_ctx = s},
        {.uri = "/config", .method = HTTP_GET, .handler = handle_config_page, .user_ctx = s},
        {.uri = "/monitor", .method = HTTP_GET, .handler = handle_monitor_page, .user_ctx = s},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = s},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get, .user_ctx = s},
        {.uri = "/api/config/save", .method = HTTP_POST, .handler = handle_config_save, .user_ctx = s},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan, .user_ctx = s},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = handle_zones, .user_ctx = s},
        {.uri = "/api/hvac/cmd", .method = HTTP_POST, .handler = handle_hvac_cmd, .user_ctx = s},
        {.uri = "/api/hvac/read", .method = HTTP_POST, .handler = handle_hvac_read, .user_ctx = s},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s->http, &routes[i]);
    }

    *out_server = s;
    ESP_LOGI(TAG, "web server started on port 80");
    return ESP_OK;
}

void web_server_stop(web_server_t *server)
{
    if (!server) {
        return;
    }
    if (server->http) {
        httpd_stop(server->http);
    }
    free(server);
}
