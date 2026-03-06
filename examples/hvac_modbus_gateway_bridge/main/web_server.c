#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telnet_server.h"

static const char *TAG = "web_server";
static const char *WEB_FS_BASE = "/spiffs";
static bool s_web_fs_ready = false;

struct web_server {
    app_config_t *cfg;
    hvac_manager_t *hvac;
    wifi_manager_t *wifi;
    ota_manager_t *ota;
    httpd_handle_t http;
};

static const char *gateway_type_to_string(hvac_gateway_type_t t)
{
    if (t == HVAC_GATEWAY_MIDEA_GW3_MOD) {
        return "midea_gw3_mod";
    }
    if (t == HVAC_GATEWAY_DAIKIN_DTA116A51) {
        return "daikin_dta116a51";
    }
    if (t == HVAC_GATEWAY_HITACHI_HCA_MB) {
        return "hitachi_hca_mb";
    }
    return "lg_pmbusb00a";
}

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

static esp_err_t handle_firmware_page(httpd_req_t *req)
{
    return send_file_page(req, "/spiffs/firmware.html");
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
    cJSON_AddStringToObject(root, "gateway_type", gateway_type_to_string(s->cfg->hvac.gateway_type));
    cJSON_AddNumberToObject(root, "idu_address_base", s->cfg->hvac.idu_address_base);
    cJSON_AddNumberToObject(root, "poll_interval_ms", s->cfg->hvac.poll_interval_ms);
    return send_json(req, root);
}

static esp_err_t handle_zones(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    char *zones = calloc(1, 4096);
    if (!zones) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (hvac_manager_zones_to_json(s->hvac, zones, 4096) != ESP_OK) {
        free(zones);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "zones encode failed");
    }

    char *out = calloc(1, 4608);
    if (!out) {
        free(zones);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    snprintf(out, 4608, "{\"ok\":true,\"zones\":%s}", zones);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    free(zones);
    return err;
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
    // If a previous scan is still running, stop it so this request is deterministic.
    esp_wifi_scan_stop();

    wifi_mode_t original_mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "get mode failed");
        cJSON_AddStringToObject(root, "err_name", esp_err_to_name(err));
        cJSON_AddNumberToObject(root, "err", err);
        cJSON_AddItemToObject(root, "items", cJSON_CreateArray());
        return send_json(req, root);
    }

    bool switched_mode = false;
    if (original_mode == WIFI_MODE_AP) {
        // In provisioning AP-only mode, scanning can fail. Temporarily enable STA.
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            switched_mode = true;
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        // Retry once after stopping scan state.
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }

    if (err != ESP_OK) {
        if (switched_mode) {
            esp_wifi_set_mode(original_mode);
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "scan failed");
        cJSON_AddStringToObject(root, "err_name", esp_err_to_name(err));
        cJSON_AddNumberToObject(root, "err", err);
        cJSON_AddItemToObject(root, "items", cJSON_CreateArray());
        return send_json(req, root);
    }

    uint16_t count = 20;
    wifi_ap_record_t recs[20] = {0};
    err = esp_wifi_scan_get_ap_records(&count, recs);
    if (err != ESP_OK) {
        if (switched_mode) {
            esp_wifi_set_mode(original_mode);
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "scan records failed");
        cJSON_AddStringToObject(root, "err_name", esp_err_to_name(err));
        cJSON_AddNumberToObject(root, "err", err);
        cJSON_AddItemToObject(root, "items", cJSON_CreateArray());
        return send_json(req, root);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(item, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "items", arr);
    if (switched_mode) {
        esp_wifi_set_mode(original_mode);
    }
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
        } else if (strcmp(gateway_type->valuestring, "hitachi_hca_mb") == 0 || strcmp(gateway_type->valuestring, "hitachi") == 0) {
            s->cfg->hvac.gateway_type = HVAC_GATEWAY_HITACHI_HCA_MB;
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

    cJSON *ota_fw = cJSON_GetObjectItemCaseSensitive(json, "ota_firmware_url");
    if (cJSON_IsString(ota_fw) && ota_fw->valuestring != NULL) {
        strlcpy(s->cfg->ota_firmware_url, ota_fw->valuestring, sizeof(s->cfg->ota_firmware_url));
    }
    cJSON *ota_fs = cJSON_GetObjectItemCaseSensitive(json, "ota_filesystem_url");
    if (cJSON_IsString(ota_fs) && ota_fs->valuestring != NULL) {
        strlcpy(s->cfg->ota_filesystem_url, ota_fs->valuestring, sizeof(s->cfg->ota_filesystem_url));
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

static esp_err_t handle_firmware_update_post(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota read failed");
        }

        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        }
        remaining -= r;
    }

    err = esp_ota_end(ota_handle);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota finalize failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    xTaskCreate(reboot_task, "fw_reboot", 2048, NULL, 4, NULL);
    return ESP_OK;
}

static esp_err_t handle_spiffs_update_post(httpd_req_t *req)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (part == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs partition not found");
    }
    if (req->content_len <= 0 || (size_t)req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid spiffs image size");
    }

    if (s_web_fs_ready) {
        esp_vfs_spiffs_unregister("spiffs");
        s_web_fs_ready = false;
    }

    const size_t erase_size = ((size_t)req->content_len + 4095U) & ~4095U;
    esp_err_t err = esp_partition_erase_range(part, 0, erase_size);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs erase failed");
    }

    char buf[1024];
    size_t offset = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs read failed");
        }
        err = esp_partition_write(part, offset, buf, (size_t)r);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "spiffs write failed");
        }
        remaining -= r;
        offset += (size_t)r;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    xTaskCreate(reboot_task, "spiffs_reboot", 2048, NULL, 4, NULL);
    return ESP_OK;
}

static esp_err_t handle_logs_get(httpd_req_t *req)
{
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
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t count = telnet_server_get_logs(since, lines, max_lines);
    uint32_t latest = since;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
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
    return send_json(req, root);
}

static esp_err_t handle_monitor_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", telnet_server_monitor_enabled());
    return send_json(req, root);
}

static esp_err_t handle_monitor_toggle_post(httpd_req_t *req)
{
    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK || body == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    if (!cJSON_IsTrue(enabled) && !cJSON_IsFalse(enabled)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled required");
    }

    telnet_server_set_monitor_enabled(cJSON_IsTrue(enabled));
    cJSON_Delete(json);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddBoolToObject(out, "enabled", telnet_server_monitor_enabled());
    return send_json(req, out);
}

static esp_err_t handle_monitor_clear_post(httpd_req_t *req)
{
    telnet_server_clear_logs();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    return send_json(req, out);
}

static esp_err_t handle_hvac_meta_get(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "gateway_type", gateway_type_to_string(s->cfg->hvac.gateway_type));

    cJSON *modes = cJSON_CreateArray();
    cJSON *fans = cJSON_CreateArray();
    cJSON *writable = cJSON_CreateArray();
    cJSON_AddItemToArray(writable, cJSON_CreateString("power"));
    cJSON_AddItemToArray(writable, cJSON_CreateString("mode"));
    cJSON_AddItemToArray(writable, cJSON_CreateString("fan"));
    cJSON_AddItemToArray(writable, cJSON_CreateString("setpoint"));

    if (s->cfg->hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD) {
        cJSON *m;
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 0); cJSON_AddStringToObject(m, "label", "cool"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 1); cJSON_AddStringToObject(m, "label", "dry"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 2); cJSON_AddStringToObject(m, "label", "fan"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 4); cJSON_AddStringToObject(m, "label", "heat"); cJSON_AddItemToArray(modes, m);
    } else {
        cJSON *m;
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 0); cJSON_AddStringToObject(m, "label", "cool"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 1); cJSON_AddStringToObject(m, "label", "dry"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 2); cJSON_AddStringToObject(m, "label", "fan"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 3); cJSON_AddStringToObject(m, "label", "auto"); cJSON_AddItemToArray(modes, m);
        m = cJSON_CreateObject(); cJSON_AddNumberToObject(m, "value", 4); cJSON_AddStringToObject(m, "label", "heat"); cJSON_AddItemToArray(modes, m);
    }

    cJSON *f;
    f = cJSON_CreateObject(); cJSON_AddNumberToObject(f, "value", 1); cJSON_AddStringToObject(f, "label", "low"); cJSON_AddItemToArray(fans, f);
    f = cJSON_CreateObject(); cJSON_AddNumberToObject(f, "value", 2); cJSON_AddStringToObject(f, "label", "mid"); cJSON_AddItemToArray(fans, f);
    f = cJSON_CreateObject(); cJSON_AddNumberToObject(f, "value", 3); cJSON_AddStringToObject(f, "label", "high"); cJSON_AddItemToArray(fans, f);
    f = cJSON_CreateObject(); cJSON_AddNumberToObject(f, "value", 4); cJSON_AddStringToObject(f, "label", "auto"); cJSON_AddItemToArray(fans, f);

    cJSON_AddItemToObject(root, "modes", modes);
    cJSON_AddItemToObject(root, "fans", fans);
    cJSON_AddItemToObject(root, "writable_fields", writable);

    cJSON_AddNumberToObject(root, "setpoint_min_tenths", s->cfg->hvac.setpoint_min_tenths);
    cJSON_AddNumberToObject(root, "setpoint_max_tenths", s->cfg->hvac.setpoint_max_tenths);

    cJSON *online = cJSON_CreateArray();
    size_t zc = hvac_manager_zone_count(s->hvac);
    for (size_t i = 0; i < zc; i++) {
        const hvac_zone_state_t *z = hvac_manager_get_zone(s->hvac, i);
        if (z == NULL || !z->connected) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", (int)i);
        cJSON_AddNumberToObject(item, "slave", z->slave_id);
        cJSON_AddNumberToObject(item, "central_address", z->central_address);
        cJSON_AddBoolToObject(item, "power", z->power);
        cJSON_AddNumberToObject(item, "mode", z->mode);
        cJSON_AddNumberToObject(item, "fan_speed", z->fan_speed);
        cJSON_AddNumberToObject(item, "setpoint_c", z->setpoint_tenths / 10.0f);
        cJSON_AddNumberToObject(item, "room_temperature_c", z->room_temp_tenths / 10.0f);
        cJSON_AddItemToArray(online, item);
    }
    cJSON_AddItemToObject(root, "online_zones", online);

    return send_json(req, root);
}

static esp_err_t handle_ota_status_get(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    if (s->ota == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota manager not available");
    }

    ota_status_t st = {0};
    ota_manager_get_status(s->ota, &st);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "active", st.active);
    cJSON_AddBoolToObject(out, "success", st.success);
    cJSON_AddBoolToObject(out, "reboot_pending", st.reboot_pending);
    cJSON_AddStringToObject(out, "target", st.target == OTA_TARGET_FIRMWARE ? "firmware" : "filesystem");
    cJSON_AddNumberToObject(out, "bytes_done", st.bytes_done);
    cJSON_AddNumberToObject(out, "bytes_total", st.bytes_total);
    cJSON_AddNumberToObject(out, "last_err", st.last_err);
    cJSON_AddStringToObject(out, "url", st.url);
    cJSON_AddStringToObject(out, "message", st.message);
    return send_json(req, out);
}

static esp_err_t handle_ota_start_post(httpd_req_t *req)
{
    web_server_t *s = (web_server_t *)req->user_ctx;
    if (s->ota == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota manager not available");
    }

    char *body = NULL;
    if (recv_body(req, &body) != ESP_OK || body == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "type required");
    }

    ota_target_t target = OTA_TARGET_FIRMWARE;
    if (strcmp(type->valuestring, "filesystem") == 0 || strcmp(type->valuestring, "fs") == 0 || strcmp(type->valuestring, "spiffs") == 0) {
        target = OTA_TARGET_FILESYSTEM;
    } else if (strcmp(type->valuestring, "firmware") != 0 && strcmp(type->valuestring, "app") != 0) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid type");
    }

    const char *url_str = (cJSON_IsString(url) && url->valuestring != NULL) ? url->valuestring : NULL;
    esp_err_t err = ota_manager_start(s->ota, target, url_str);
    cJSON_Delete(json);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(out, "err", err);
    cJSON_AddStringToObject(out, "err_name", esp_err_to_name(err));
    if (err == ESP_OK) {
        cJSON_AddStringToObject(out, "msg", "OTA started");
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddStringToObject(out, "msg", "OTA already running");
    } else if (err == ESP_ERR_INVALID_ARG) {
        cJSON_AddStringToObject(out, "msg", "invalid type/url");
    } else {
        cJSON_AddStringToObject(out, "msg", "failed to start OTA");
    }
    return send_json(req, out);
}

esp_err_t web_server_start(web_server_t **out_server, const web_server_config_t *cfg)
{
    if (!out_server || !cfg || !cfg->cfg || !cfg->hvac || !cfg->wifi || !cfg->ota) {
        return ESP_ERR_INVALID_ARG;
    }

    web_server_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->cfg = cfg->cfg;
    s->hvac = cfg->hvac;
    s->wifi = cfg->wifi;
    s->ota = cfg->ota;

    esp_err_t err = mount_web_fs();
    if (err != ESP_OK) {
        free(s);
        return err;
    }

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 32;
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
        {.uri = "/firmware", .method = HTTP_GET, .handler = handle_firmware_page, .user_ctx = s},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = s},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_config_get, .user_ctx = s},
        {.uri = "/api/config/save", .method = HTTP_POST, .handler = handle_config_save, .user_ctx = s},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handle_wifi_scan, .user_ctx = s},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs_get, .user_ctx = s},
        {.uri = "/api/monitor", .method = HTTP_GET, .handler = handle_monitor_get, .user_ctx = s},
        {.uri = "/api/monitor/toggle", .method = HTTP_POST, .handler = handle_monitor_toggle_post, .user_ctx = s},
        {.uri = "/api/monitor/clear", .method = HTTP_POST, .handler = handle_monitor_clear_post, .user_ctx = s},
        {.uri = "/api/hvac/meta", .method = HTTP_GET, .handler = handle_hvac_meta_get, .user_ctx = s},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = handle_zones, .user_ctx = s},
        {.uri = "/api/hvac/cmd", .method = HTTP_POST, .handler = handle_hvac_cmd, .user_ctx = s},
        {.uri = "/api/hvac/read", .method = HTTP_POST, .handler = handle_hvac_read, .user_ctx = s},
        {.uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status_get, .user_ctx = s},
        {.uri = "/api/ota/start", .method = HTTP_POST, .handler = handle_ota_start_post, .user_ctx = s},
        {.uri = "/firmware/update", .method = HTTP_POST, .handler = handle_firmware_update_post, .user_ctx = s},
        {.uri = "/spiffs/update", .method = HTTP_POST, .handler = handle_spiffs_update_post, .user_ctx = s},
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
