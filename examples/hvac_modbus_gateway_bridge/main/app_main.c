#include <stdio.h>

#include "address_map.h"
#include "app_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "hvac.h"
#include "modbus_client.h"
#include "ota_manager.h"
#include "telnet_server.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    static app_config_t cfg;
    ESP_ERROR_CHECK(app_config_load(&cfg));

    esp_log_level_set("*", cfg.system.log_level);

    ESP_LOGI(TAG, "Device ID: %s", cfg.system.device_id);
    ESP_LOGI(TAG, "Zone formula check: N=0 power=%u mode=%u setpoint=%u", get_coil_address(0, 1), get_holding_address(0, 1), get_holding_address(0, 3));
    ESP_LOGI(TAG, "Zone formula check: N=2 power=%u mode=%u setpoint=%u", get_coil_address(2, 1), get_holding_address(2, 1), get_holding_address(2, 3));

    wifi_manager_t *wifi = NULL;
    wifi_manager_config_t wifi_cfg = {
        .cfg = &cfg,
    };
    ESP_ERROR_CHECK(wifi_manager_init(&wifi, &wifi_cfg));

    modbus_client_t *modbus = NULL;
    modbus_client_config_t mb_cfg = {
        .uart_num = (uart_port_t)cfg.modbus.uart_num,
        .tx_pin = cfg.modbus.tx_pin,
        .rx_pin = cfg.modbus.rx_pin,
        .de_pin = cfg.modbus.de_pin,
        .baud = cfg.modbus.baud,
        .parity = cfg.modbus.parity,
        .stop_bits = cfg.modbus.stop_bits,
        .timeout_ms = cfg.modbus.timeout_ms,
        .retries = cfg.modbus.retries,
        .use_hw_rs485 = cfg.modbus.use_hw_rs485,
    };
    ESP_ERROR_CHECK(modbus_client_init(&modbus, &mb_cfg));

    hvac_manager_t *hvac = NULL;
    ESP_ERROR_CHECK(hvac_manager_init(&hvac, &cfg, modbus));
    hvac_manager_start(hvac);

    telnet_server_t *telnet = NULL;
    telnet_server_config_t tel_cfg = {
        .cfg = &cfg,
        .hvac = hvac,
        .wifi = wifi,
    };
    ESP_ERROR_CHECK(telnet_server_start(&telnet, &tel_cfg));

    web_server_t *web = NULL;
    ota_manager_t *ota = NULL;
    ota_manager_config_t ota_cfg = {
        .cfg = &cfg,
    };
    ESP_ERROR_CHECK(ota_manager_init(&ota, &ota_cfg));

    web_server_config_t web_cfg = {
        .cfg = &cfg,
        .hvac = hvac,
        .wifi = wifi,
        .ota = ota,
    };
    ESP_ERROR_CHECK(web_server_start(&web, &web_cfg));

    ESP_LOGI(TAG, "System initialized. Telnet API on port 23 and Web API on port 80.");
}
