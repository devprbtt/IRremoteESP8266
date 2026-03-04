#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_manager";

static bool s_sta_connected = false;
static esp_netif_t *s_netif_sta = NULL;
static TaskHandle_t s_reconnect_task = NULL;
static bool s_sta_cfg_set = false;

static void wifi_reconnect_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_sta_cfg_set && !s_sta_connected) {
            ESP_LOGW(TAG, "STA not connected, retrying connect");
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        esp_wifi_connect();
        ESP_LOGW(TAG, "STA disconnected, reconnecting");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA got IP");
    }
}

esp_err_t wifi_manager_start_apsta(const char *ap_ssid, const char *ap_password, const char *hostname) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1U);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    if (ap_password != NULL && ap_password[0] != '\0') {
        strncpy((char *)ap_cfg.ap.password, ap_password, sizeof(ap_cfg.ap.password) - 1U);
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    if (hostname != NULL && hostname[0] != '\0') {
        esp_netif_set_hostname(s_netif_sta, hostname);
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "AP started: ssid=%s", ap_ssid);
    if (s_reconnect_task == NULL) {
        xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 2048, NULL, 4, &s_reconnect_task);
    }
    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_network(bool dhcp, const char *ip, const char *gateway, const char *subnet, const char *dns) {
    if (s_netif_sta == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (dhcp) {
        esp_err_t err = esp_netif_dhcpc_stop(s_netif_sta);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            return err;
        }
        err = esp_netif_dhcpc_start(s_netif_sta);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return ESP_OK;
        }
        return err;
    }

    if (ip == NULL || gateway == NULL || subnet == NULL || ip[0] == '\0' || gateway[0] == '\0' || subnet[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (!esp_netif_str_to_ip4(ip, &ip_info.ip) ||
        !esp_netif_str_to_ip4(gateway, &ip_info.gw) ||
        !esp_netif_str_to_ip4(subnet, &ip_info.netmask)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif_sta);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    err = esp_netif_set_ip_info(s_netif_sta, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    if (dns != NULL && dns[0] != '\0') {
        esp_netif_dns_info_t dns_info = {0};
        if (!esp_netif_str_to_ip4(dns, &dns_info.ip.u_addr.ip4)) {
            return ESP_ERR_INVALID_ARG;
        }
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        err = esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    return err;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sta_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1U);
    if (password != NULL) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1U);
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_sta_cfg_set = true;
    return esp_wifi_connect();
}

bool wifi_manager_sta_connected(void) {
    return s_sta_connected;
}

esp_err_t wifi_manager_sta_ip(char *out, size_t out_len) {
    if (out == NULL || out_len == 0U || s_netif_sta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_netif_sta, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_scan_item_t *items, size_t max_items, size_t *out_count) {
    if (items == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        return err;
    }
    if (ap_num == 0U) {
        return ESP_OK;
    }

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_num, recs);
    if (err != ESP_OK) {
        free(recs);
        return err;
    }

    size_t n = ap_num < max_items ? ap_num : max_items;
    for (size_t i = 0; i < n; i++) {
        strncpy(items[i].ssid, (const char *)recs[i].ssid, sizeof(items[i].ssid) - 1U);
        items[i].ssid[sizeof(items[i].ssid) - 1U] = '\0';
        items[i].rssi = recs[i].rssi;
        items[i].authmode = recs[i].authmode;
    }
    *out_count = n;

    free(recs);
    return ESP_OK;
}
