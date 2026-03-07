#include "app_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_config";
static const char *NVS_NS = "cfg";

static esp_err_t appendf(char *out, size_t out_len, size_t *offset, const char *fmt, ...)
{
    if (*offset >= out_len) {
        return ESP_ERR_NO_MEM;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *offset, out_len - *offset, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= out_len - *offset) {
        return ESP_ERR_NO_MEM;
    }

    *offset += (size_t)written;
    return ESP_OK;
}

void app_config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strlcpy(cfg->system.device_id, "hvac-modbus-gateway-bridge", sizeof(cfg->system.device_id));
    cfg->system.log_level = ESP_LOG_INFO;

    cfg->modbus.uart_num = 2;
    cfg->modbus.tx_pin = 17;
    cfg->modbus.rx_pin = 16;
    cfg->modbus.de_pin = 4;
    cfg->modbus.baud = 9600;
    cfg->modbus.parity = 0;
    cfg->modbus.stop_bits = 1;
    cfg->modbus.timeout_ms = 300;
    cfg->modbus.retries = 3;
    cfg->modbus.default_slave_id = 1;
    cfg->modbus.use_hw_rs485 = true;

    cfg->hvac.gateway_type = HVAC_GATEWAY_LG_PMBUSB00A;
    cfg->hvac.idu_address_base = 0;
    cfg->hvac.poll_interval_ms = 3000;
    cfg->hvac.mode_rate_limit_ms = 10000;
    cfg->hvac.setpoint_min_tenths = 160;
    cfg->hvac.setpoint_max_tenths = 300;
    cfg->hvac.zone_count = 3;
    cfg->hvac.zones[0].slave_id = 1;
    cfg->hvac.zones[0].central_address = 0;
    cfg->hvac.zones[1].slave_id = 1;
    cfg->hvac.zones[1].central_address = 1;
    cfg->hvac.zones[2].slave_id = 1;
    cfg->hvac.zones[2].central_address = 2;

    cfg->ota_firmware_url[0] = '\0';
    cfg->ota_filesystem_url[0] = '\0';
}

esp_err_t app_config_set_wifi(app_config_t *cfg, const char *ssid, const char *password)
{
    if (!cfg || !ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(cfg->wifi.ssid, ssid, sizeof(cfg->wifi.ssid));
    strlcpy(cfg->wifi.password, password, sizeof(cfg->wifi.password));
    return ESP_OK;
}

esp_err_t app_config_set_zones_from_csv(app_config_t *cfg, const char *csv)
{
    if (!cfg || !csv) {
        return ESP_ERR_INVALID_ARG;
    }

    char *work = strdup(csv);
    if (!work) {
        return ESP_ERR_NO_MEM;
    }

    zone_cfg_t parsed[APP_CFG_MAX_ZONES] = {0};
    size_t count = 0;
    char *saveptr = NULL;

    for (char *tok = strtok_r(work, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ') {
            tok++;
        }

        char *end = tok + strlen(tok) - 1;
        while (end >= tok && *end == ' ') {
            *end-- = '\0';
        }

        if (*tok == '\0') {
            continue;
        }

        if (count >= APP_CFG_MAX_ZONES) {
            free(work);
            return ESP_ERR_INVALID_SIZE;
        }

        zone_cfg_t z = {
            .slave_id = cfg->modbus.default_slave_id,
            .central_address = 0,
        };

        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            long slave = strtol(tok, NULL, 10);
            long ca = strtol(colon + 1, NULL, 10);
            if (slave < 1 || slave > 247 || ca < 0 || ca > 2047) {
                free(work);
                return ESP_ERR_INVALID_ARG;
            }
            z.slave_id = (uint8_t)slave;
            z.central_address = (uint16_t)ca;
        } else {
            long ca = strtol(tok, NULL, 10);
            if (ca < 0 || ca > 2047) {
                free(work);
                return ESP_ERR_INVALID_ARG;
            }
            z.central_address = (uint16_t)ca;
        }

        parsed[count++] = z;
    }

    free(work);

    if (count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cfg->hvac.zone_count = count;
    memcpy(cfg->hvac.zones, parsed, sizeof(zone_cfg_t) * count);
    return ESP_OK;
}

esp_err_t app_config_get_zones_csv(const app_config_t *cfg, char *out, size_t out_len)
{
    if (!cfg || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    out[0] = '\0';
    for (size_t i = 0; i < cfg->hvac.zone_count; i++) {
        const zone_cfg_t *z = &cfg->hvac.zones[i];
        esp_err_t err = appendf(out, out_len, &offset, "%s%u:%u", (i == 0) ? "" : ",", z->slave_id, z->central_address);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t nvs_get_str_into(nvs_handle_t h, const char *key, char *dest, size_t dest_len)
{
    size_t len = dest_len;
    esp_err_t err = nvs_get_str(h, key, dest, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static void nvs_get_i32_into_int(nvs_handle_t h, const char *key, int *dest)
{
    if (!dest) {
        return;
    }

    int32_t v = (int32_t)*dest;
    if (nvs_get_i32(h, key, &v) == ESP_OK) {
        *dest = (int)v;
    }
}

esp_err_t app_config_load(app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS namespace not found, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_get_i32_into_int(h, "log_level", &cfg->system.log_level);

    nvs_get_i32_into_int(h, "uart_num", &cfg->modbus.uart_num);
    nvs_get_i32_into_int(h, "tx_pin", &cfg->modbus.tx_pin);
    nvs_get_i32_into_int(h, "rx_pin", &cfg->modbus.rx_pin);
    nvs_get_i32_into_int(h, "de_pin", &cfg->modbus.de_pin);
    nvs_get_i32_into_int(h, "baud", &cfg->modbus.baud);
    nvs_get_i32_into_int(h, "mb_parity", &cfg->modbus.parity);
    nvs_get_i32_into_int(h, "mb_stop", &cfg->modbus.stop_bits);
    nvs_get_i32_into_int(h, "mb_to_ms", &cfg->modbus.timeout_ms);
    nvs_get_i32_into_int(h, "mb_retries", &cfg->modbus.retries);
    nvs_get_u8(h, "mb_slave", &cfg->modbus.default_slave_id);

    uint8_t hw = cfg->modbus.use_hw_rs485 ? 1 : 0;
    nvs_get_u8(h, "mb_hw485", &hw);
    cfg->modbus.use_hw_rs485 = (hw != 0);

    nvs_get_i32_into_int(h, "idu_base", &cfg->hvac.idu_address_base);
    uint8_t gw = (uint8_t)cfg->hvac.gateway_type;
    nvs_get_u8(h, "gw_type", &gw);
    if (gw == 1) {
        cfg->hvac.gateway_type = HVAC_GATEWAY_MIDEA_GW3_MOD;
    } else if (gw == 2) {
        cfg->hvac.gateway_type = HVAC_GATEWAY_DAIKIN_DTA116A51;
    } else if (gw == 3) {
        cfg->hvac.gateway_type = HVAC_GATEWAY_HITACHI_HCA_MB;
    } else if (gw == 4) {
        cfg->hvac.gateway_type = HVAC_GATEWAY_SAMSUNG_MIM_B19N;
    } else {
        cfg->hvac.gateway_type = HVAC_GATEWAY_LG_PMBUSB00A;
    }
    nvs_get_i32_into_int(h, "poll_ms", &cfg->hvac.poll_interval_ms);
    nvs_get_i32_into_int(h, "mode_rl_ms", &cfg->hvac.mode_rate_limit_ms);
    nvs_get_i32_into_int(h, "sp_min", &cfg->hvac.setpoint_min_tenths);
    nvs_get_i32_into_int(h, "sp_max", &cfg->hvac.setpoint_max_tenths);

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_str_into(h, "dev_id", cfg->system.device_id, sizeof(cfg->system.device_id)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_str_into(h, "wifi_ssid", cfg->wifi.ssid, sizeof(cfg->wifi.ssid)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_get_str_into(h, "wifi_pass", cfg->wifi.password, sizeof(cfg->wifi.password)));

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        nvs_get_str_into(h, "ota_fw_url", cfg->ota_firmware_url, sizeof(cfg->ota_firmware_url)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        nvs_get_str_into(h, "ota_fs_url", cfg->ota_filesystem_url, sizeof(cfg->ota_filesystem_url)));

    // Backward compatibility with old single OTA URL key.
    if (cfg->ota_firmware_url[0] == '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            nvs_get_str_into(h, "ota_url", cfg->ota_firmware_url, sizeof(cfg->ota_firmware_url)));
    }

    char zones_csv[256] = {0};
    if (nvs_get_str_into(h, "zones", zones_csv, sizeof(zones_csv)) == ESP_OK && zones_csv[0] != '\0') {
        ESP_ERROR_CHECK_WITHOUT_ABORT(app_config_set_zones_from_csv(cfg, zones_csv));
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char zones_csv[256] = {0};
    err = app_config_get_zones_csv(cfg, zones_csv, sizeof(zones_csv));
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    nvs_set_i32(h, "log_level", cfg->system.log_level);
    nvs_set_i32(h, "uart_num", cfg->modbus.uart_num);
    nvs_set_i32(h, "tx_pin", cfg->modbus.tx_pin);
    nvs_set_i32(h, "rx_pin", cfg->modbus.rx_pin);
    nvs_set_i32(h, "de_pin", cfg->modbus.de_pin);
    nvs_set_i32(h, "baud", cfg->modbus.baud);
    nvs_set_i32(h, "mb_parity", cfg->modbus.parity);
    nvs_set_i32(h, "mb_stop", cfg->modbus.stop_bits);
    nvs_set_i32(h, "mb_to_ms", cfg->modbus.timeout_ms);
    nvs_set_i32(h, "mb_retries", cfg->modbus.retries);
    nvs_set_u8(h, "mb_slave", cfg->modbus.default_slave_id);
    nvs_set_u8(h, "mb_hw485", cfg->modbus.use_hw_rs485 ? 1 : 0);

    nvs_set_i32(h, "idu_base", cfg->hvac.idu_address_base);
    nvs_set_u8(h, "gw_type", (uint8_t)cfg->hvac.gateway_type);
    nvs_set_i32(h, "poll_ms", cfg->hvac.poll_interval_ms);
    nvs_set_i32(h, "mode_rl_ms", cfg->hvac.mode_rate_limit_ms);
    nvs_set_i32(h, "sp_min", cfg->hvac.setpoint_min_tenths);
    nvs_set_i32(h, "sp_max", cfg->hvac.setpoint_max_tenths);

    nvs_set_str(h, "dev_id", cfg->system.device_id);
    nvs_set_str(h, "wifi_ssid", cfg->wifi.ssid);
    nvs_set_str(h, "wifi_pass", cfg->wifi.password);
    nvs_set_str(h, "zones", zones_csv);
    nvs_set_str(h, "ota_fw_url", cfg->ota_firmware_url);
    nvs_set_str(h, "ota_fs_url", cfg->ota_filesystem_url);

    // Keep legacy key mirrored to firmware URL for downgrade compatibility.
    nvs_set_str(h, "ota_url", cfg->ota_firmware_url);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_to_json(const app_config_t *cfg, char *out, size_t out_len)
{
    if (!cfg || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    esp_err_t err = appendf(out,
                            out_len,
                            &offset,
                            "{\"device_id\":\"%s\",\"log_level\":%d,\"wifi\":{\"ssid\":\"%s\"},"
                            "\"modbus\":{\"uart\":%d,\"tx\":%d,\"rx\":%d,\"de\":%d,\"baud\":%d,\"parity\":%d,\"stop_bits\":%d,\"timeout_ms\":%d,\"retries\":%d,\"default_slave\":%u,\"use_hw_rs485\":%s},"
                            "\"hvac\":{\"gateway_type\":\"%s\",\"idu_address_base\":%d,\"poll_interval_ms\":%d,\"mode_rate_limit_ms\":%d,\"setpoint_min_tenths\":%d,\"setpoint_max_tenths\":%d,\"zones\":[",
                            cfg->system.device_id,
                            cfg->system.log_level,
                            cfg->wifi.ssid,
                            cfg->modbus.uart_num,
                            cfg->modbus.tx_pin,
                            cfg->modbus.rx_pin,
                            cfg->modbus.de_pin,
                            cfg->modbus.baud,
                            cfg->modbus.parity,
                            cfg->modbus.stop_bits,
                            cfg->modbus.timeout_ms,
                            cfg->modbus.retries,
                            cfg->modbus.default_slave_id,
                            cfg->modbus.use_hw_rs485 ? "true" : "false",
                            cfg->hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD
                                ? "midea_gw3_mod"
                                : (cfg->hvac.gateway_type == HVAC_GATEWAY_DAIKIN_DTA116A51
                                       ? "daikin_dta116a51"
                                       : (cfg->hvac.gateway_type == HVAC_GATEWAY_HITACHI_HCA_MB
                                              ? "hitachi_hca_mb"
                                              : (cfg->hvac.gateway_type == HVAC_GATEWAY_SAMSUNG_MIM_B19N
                                                     ? "samsung_mim_b19n"
                                                     : "lg_pmbusb00a"))),
                            cfg->hvac.idu_address_base,
                            cfg->hvac.poll_interval_ms,
                            cfg->hvac.mode_rate_limit_ms,
                            cfg->hvac.setpoint_min_tenths,
                            cfg->hvac.setpoint_max_tenths);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < cfg->hvac.zone_count; i++) {
        err = appendf(out,
                      out_len,
                      &offset,
                      "%s{\"slave\":%u,\"central_address\":%u}",
                      (i == 0) ? "" : ",",
                      cfg->hvac.zones[i].slave_id,
                      cfg->hvac.zones[i].central_address);
        if (err != ESP_OK) {
            return err;
        }
    }

    return appendf(out,
                   out_len,
                   &offset,
                   "]},\"ota\":{\"firmware_url\":\"%s\",\"filesystem_url\":\"%s\"}}",
                   cfg->ota_firmware_url,
                   cfg->ota_filesystem_url);
}
