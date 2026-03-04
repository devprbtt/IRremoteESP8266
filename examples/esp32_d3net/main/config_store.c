#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "modbus_rtu.h"

static const char *TAG = "config_store";
static const char *DEFAULT_HOSTNAME = "daikin-d3net";
static const uint16_t DEFAULT_TELNET_PORT = 23;

static void set_network_defaults(app_config_t *cfg) {
    cfg->sta_dhcp = true;
    cfg->sta_ip[0] = '\0';
    cfg->sta_gateway[0] = '\0';
    cfg->sta_subnet[0] = '\0';
    cfg->sta_dns[0] = '\0';
    strncpy(cfg->hostname, DEFAULT_HOSTNAME, sizeof(cfg->hostname) - 1U);
    cfg->hostname[sizeof(cfg->hostname) - 1U] = '\0';
    cfg->telnet_port = DEFAULT_TELNET_PORT;
    cfg->din_gateway_host[0] = '\0';
    cfg->din_auto_connect = false;
    cfg->din_actions_json[0] = '\0';
}

static void set_rtu_defaults(app_config_t *cfg) {
    cfg->rtu_cfg.uart_num = UART_NUM_1;
    cfg->rtu_cfg.tx_pin = 17;
    cfg->rtu_cfg.rx_pin = 16;
    cfg->rtu_cfg.de_pin = 4;
    cfg->rtu_cfg.re_pin = 5;
    cfg->rtu_cfg.baud_rate = 19200;
    cfg->rtu_cfg.data_bits = 8;
    cfg->rtu_cfg.stop_bits = 2;
    cfg->rtu_cfg.parity = 'N';
    cfg->rtu_cfg.slave_id = 1;
    cfg->rtu_cfg.timeout_ms = 3000;
}

esp_err_t config_store_load(app_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(*cfg));
    set_network_defaults(cfg);
    set_rtu_defaults(cfg);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("d3net", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(cfg->sta_ssid);
    err = nvs_get_str(nvs, "sta_ssid", cfg->sta_ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return ESP_OK;
    }

    size_t pass_len = sizeof(cfg->sta_password);
    err = nvs_get_str(nvs, "sta_pass", cfg->sta_password, &pass_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        cfg->sta_password[0] = '\0';
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        cfg->sta_configured = cfg->sta_ssid[0] != '\0';
    }
    uint8_t sta_dhcp = 1U;
    if (nvs_get_u8(nvs, "sta_dhcp", &sta_dhcp) == ESP_OK) {
        cfg->sta_dhcp = sta_dhcp != 0U;
    }
    size_t ip_len = sizeof(cfg->sta_ip);
    if (nvs_get_str(nvs, "sta_ip", cfg->sta_ip, &ip_len) != ESP_OK) {
        cfg->sta_ip[0] = '\0';
    }
    size_t gw_len = sizeof(cfg->sta_gateway);
    if (nvs_get_str(nvs, "sta_gw", cfg->sta_gateway, &gw_len) != ESP_OK) {
        cfg->sta_gateway[0] = '\0';
    }
    size_t sub_len = sizeof(cfg->sta_subnet);
    if (nvs_get_str(nvs, "sta_subnet", cfg->sta_subnet, &sub_len) != ESP_OK) {
        cfg->sta_subnet[0] = '\0';
    }
    size_t dns_len = sizeof(cfg->sta_dns);
    if (nvs_get_str(nvs, "sta_dns", cfg->sta_dns, &dns_len) != ESP_OK) {
        cfg->sta_dns[0] = '\0';
    }
    size_t host_len = sizeof(cfg->hostname);
    if (nvs_get_str(nvs, "hostname", cfg->hostname, &host_len) != ESP_OK || cfg->hostname[0] == '\0') {
        strncpy(cfg->hostname, DEFAULT_HOSTNAME, sizeof(cfg->hostname) - 1U);
        cfg->hostname[sizeof(cfg->hostname) - 1U] = '\0';
    }
    uint16_t telnet_port = DEFAULT_TELNET_PORT;
    if (nvs_get_u16(nvs, "tel_port", &telnet_port) == ESP_OK && telnet_port > 0U) {
        cfg->telnet_port = telnet_port;
    }
    size_t din_host_len = sizeof(cfg->din_gateway_host);
    if (nvs_get_str(nvs, "din_host", cfg->din_gateway_host, &din_host_len) != ESP_OK) {
        cfg->din_gateway_host[0] = '\0';
    }
    uint8_t din_auto = 0U;
    if (nvs_get_u8(nvs, "din_auto", &din_auto) == ESP_OK) {
        cfg->din_auto_connect = din_auto != 0U;
    }
    size_t din_actions_len = sizeof(cfg->din_actions_json);
    if (nvs_get_str(nvs, "din_actions", cfg->din_actions_json, &din_actions_len) != ESP_OK) {
        cfg->din_actions_json[0] = '\0';
    }

    uint64_t mask = 0;
    size_t ids_len = sizeof(cfg->registered_ids);
    if (nvs_get_u64(nvs, "reg_mask", &mask) == ESP_OK) {
        cfg->registered_mask = mask;
    }
    if (nvs_get_blob(nvs, "reg_ids", cfg->registered_ids, &ids_len) != ESP_OK || ids_len != sizeof(cfg->registered_ids)) {
        memset(cfg->registered_ids, 0, sizeof(cfg->registered_ids));
    }

    size_t rtu_len = sizeof(cfg->rtu_cfg);
    if (nvs_get_blob(nvs, "rtu_cfg", &cfg->rtu_cfg, &rtu_len) != ESP_OK || rtu_len != sizeof(cfg->rtu_cfg)) {
        set_rtu_defaults(cfg);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t config_store_save(const app_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("d3net", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "sta_ssid", cfg->sta_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_pass", cfg->sta_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "sta_dhcp", cfg->sta_dhcp ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_ip", cfg->sta_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_gw", cfg->sta_gateway);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_subnet", cfg->sta_subnet);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "sta_dns", cfg->sta_dns);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "hostname", cfg->hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "tel_port", cfg->telnet_port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "din_host", cfg->din_gateway_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "din_auto", cfg->din_auto_connect ? 1U : 0U);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "din_actions", cfg->din_actions_json);
    }
    if (err == ESP_OK) {
        err = nvs_set_u64(nvs, "reg_mask", cfg->registered_mask);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "reg_ids", cfg->registered_ids, sizeof(cfg->registered_ids));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "rtu_cfg", &cfg->rtu_cfg, sizeof(cfg->rtu_cfg));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved Wi-Fi config for SSID '%s'", cfg->sta_ssid);
    }
    return err;
}
