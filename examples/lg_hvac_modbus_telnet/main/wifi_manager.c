#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi_mgr";

struct wifi_manager {
    app_config_t *cfg;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    EventGroupHandle_t events;
    bool connected;
    bool ap_mode;
    char ap_ssid[33];
};

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)event_data;
    wifi_manager_t *mgr = (wifi_manager_t *)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        mgr->connected = false;
        xEventGroupClearBits(mgr->events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        mgr->connected = true;
        xEventGroupSetBits(mgr->events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi connected");
    }
}

static esp_err_t start_sta(wifi_manager_t *mgr)
{
    wifi_config_t wcfg = {0};
    strlcpy((char *)wcfg.sta.ssid, mgr->cfg->wifi.ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, mgr->cfg->wifi.password, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wcfg), TAG, "set sta config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    mgr->ap_mode = false;
    mgr->ap_ssid[0] = '\0';
    ESP_LOGI(TAG, "connecting to SSID: %s", mgr->cfg->wifi.ssid);
    return ESP_OK;
}

static esp_err_t start_provision_ap(wifi_manager_t *mgr)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    snprintf(mgr->ap_ssid, sizeof(mgr->ap_ssid), "LGHVAC-SETUP-%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, mgr->ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(mgr->ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode ap failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start ap failed");

    mgr->ap_mode = true;
    ESP_LOGW(TAG, "Wi-Fi not configured. Join SSID '%s' and open http://192.168.4.1", mgr->ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_init(wifi_manager_t **out_mgr, const wifi_manager_config_t *cfg)
{
    if (!out_mgr || !cfg || !cfg->cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;

    wifi_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) {
        return ESP_ERR_NO_MEM;
    }
    mgr->cfg = cfg->cfg;
    mgr->events = xEventGroupCreate();
    if (!mgr->events) {
        free(mgr);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ret = err;
        goto fail;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ret = err;
        goto fail;
    }

    mgr->sta_netif = esp_netif_create_default_wifi_sta();
    mgr->ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wicfg);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, mgr);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, mgr);
    if (ret != ESP_OK) {
        goto fail;
    }

    if (mgr->cfg->wifi.ssid[0] != '\0') {
        ret = start_sta(mgr);
    } else {
        ret = start_provision_ap(mgr);
    }
    if (ret != ESP_OK) {
        goto fail;
    }

    *out_mgr = mgr;
    return ESP_OK;

fail:
    if (mgr->events) {
        vEventGroupDelete(mgr->events);
    }
    free(mgr);
    return ret;
}

bool wifi_manager_is_connected(const wifi_manager_t *mgr)
{
    return mgr && mgr->connected;
}

esp_err_t wifi_manager_get_ip(const wifi_manager_t *mgr, char *buf, size_t buf_len)
{
    if (!mgr || !buf || buf_len < 8 || !mgr->sta_netif) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip = {0};
    esp_err_t err = esp_netif_get_ip_info(mgr->sta_netif, &ip);
    if (err != ESP_OK) {
        return err;
    }

    const char *s = ip4addr_ntoa((const ip4_addr_t *)&ip.ip);
    if (!s) {
        return ESP_FAIL;
    }
    strlcpy(buf, s, buf_len);
    return ESP_OK;
}

bool wifi_manager_is_ap_mode(const wifi_manager_t *mgr)
{
    return mgr && mgr->ap_mode;
}

esp_err_t wifi_manager_get_ap_ssid(const wifi_manager_t *mgr, char *buf, size_t buf_len)
{
    if (!mgr || !buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(buf, mgr->ap_ssid, buf_len);
    return ESP_OK;
}
