idf_component_register(
    SRCS
        "app_main.c"
        "app_config.c"
        "address_map.c"
        "modbus_client.c"
        "hvac.c"
        "wifi_manager.c"
        "telnet_server.c"
        "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server driver lwip esp_timer modbus json spiffs
)
