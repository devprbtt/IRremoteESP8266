#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "app_context.h"
#include "config_store.h"
#include "d3net_gateway.h"
#include "modbus_rtu.h"
#include "telnet_server.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "d3net_main";
static const char *DEFAULT_HOSTNAME = "daikin-d3net";

typedef struct {
    modbus_rtu_ctx_t rtu;
} transport_ctx_t;

static app_context_t s_app;

static void start_mdns(const app_context_t *app) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    const char *hostname = (app->config.hostname[0] != '\0') ? app->config.hostname : DEFAULT_HOSTNAME;
    uint16_t telnet_port = (app->config.telnet_port == 0U) ? 23U : app->config.telnet_port;
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set("Daikin D3Net Controller"));
    ESP_ERROR_CHECK(mdns_service_add("Daikin D3Net Web", "_http", "_tcp", 80, NULL, 0));
    ESP_ERROR_CHECK(mdns_service_add("Daikin D3Net Telnet", "_telnet", "_tcp", telnet_port, NULL, 0));

    ESP_LOGI(TAG, "mDNS started: http://%s.local", hostname);
}

static esp_err_t rtu_read_registers(
    void *ctx,
    d3net_reg_type_t reg_type,
    uint16_t address,
    uint16_t count,
    uint16_t *out_words) {
    transport_ctx_t *transport = (transport_ctx_t *)ctx;
    return modbus_rtu_read_registers(&transport->rtu, reg_type, address, count, out_words);
}

static esp_err_t rtu_write_registers(
    void *ctx,
    uint16_t address,
    uint16_t count,
    const uint16_t *words) {
    transport_ctx_t *transport = (transport_ctx_t *)ctx;
    return modbus_rtu_write_registers(&transport->rtu, address, count, words);
}

static void poll_task(void *arg) {
    app_context_t *app = (app_context_t *)arg;
    while (true) {
        if (xSemaphoreTake(app->gateway_lock, pdMS_TO_TICKS(4000)) == pdTRUE) {
            static bool first = true;
            esp_err_t err = ESP_OK;

            if (first) {
                err = d3net_gateway_discover_units(&app->gateway);
                if (err == ESP_OK) {
                    telnet_server_logf("discovered %u units", app->gateway.discovered_count);
                    first = false;
                } else {
                    telnet_server_logf("discover failed: %s", esp_err_to_name(err));
                }
            } else {
                err = d3net_gateway_poll_status(&app->gateway);
                if (err != ESP_OK) {
                    telnet_server_logf("poll failed: %s", esp_err_to_name(err));
                }
            }
            xSemaphoreGive(app->gateway_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(app->gateway.poll_interval_s * 1000U));
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    memset(&s_app, 0, sizeof(s_app));
    s_app.gateway_lock = xSemaphoreCreateMutex();
    strncpy(s_app.ota.message, "idle", sizeof(s_app.ota.message) - 1U);

    err = config_store_load(&s_app.config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config load failed: %s", esp_err_to_name(err));
    }

    if (s_app.config.hostname[0] == '\0') {
        strncpy(s_app.config.hostname, DEFAULT_HOSTNAME, sizeof(s_app.config.hostname) - 1U);
        s_app.config.hostname[sizeof(s_app.config.hostname) - 1U] = '\0';
    }
    if (s_app.config.telnet_port == 0U) {
        s_app.config.telnet_port = 23U;
    }

    ESP_ERROR_CHECK(wifi_manager_start_apsta("DaikinD3Net-Setup", "daikinsetup", s_app.config.hostname));
    err = wifi_manager_set_sta_network(
        s_app.config.sta_dhcp,
        s_app.config.sta_ip,
        s_app.config.sta_gateway,
        s_app.config.sta_subnet,
        s_app.config.sta_dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA network config failed, fallback to DHCP: %s", esp_err_to_name(err));
        s_app.config.sta_dhcp = true;
        (void)wifi_manager_set_sta_network(true, NULL, NULL, NULL, NULL);
    }
    if (s_app.config.sta_configured) {
        err = wifi_manager_connect_sta(s_app.config.sta_ssid, s_app.config.sta_password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saved STA connect failed: %s", esp_err_to_name(err));
        }
    }

    transport_ctx_t *transport = calloc(1, sizeof(*transport));
    if (transport == NULL) {
        ESP_LOGE(TAG, "transport alloc failed");
        return;
    }

    modbus_rtu_config_t rtu_cfg = s_app.config.rtu_cfg;
    err = modbus_rtu_init(&transport->rtu, &rtu_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtu init failed: %s", esp_err_to_name(err));
        return;
    }

    d3net_gateway_init(&s_app.gateway, rtu_read_registers, rtu_write_registers, transport, rtu_cfg.slave_id);

    httpd_handle_t web = NULL;
    ESP_ERROR_CHECK(web_server_start(&s_app, &web));
    ESP_ERROR_CHECK(telnet_server_start(&s_app));
    start_mdns(&s_app);
    xTaskCreate(poll_task, "d3net_poll", 6144, &s_app, 5, NULL);

    ESP_LOGI(TAG, "system started: AP setup SSID=DaikinD3Net-Setup");
}
