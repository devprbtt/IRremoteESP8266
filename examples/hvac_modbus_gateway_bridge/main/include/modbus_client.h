#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int de_pin;
    int baud;
    int timeout_ms;
    int retries;
    bool use_hw_rs485;
} modbus_client_config_t;

typedef struct modbus_client modbus_client_t;

esp_err_t modbus_client_init(modbus_client_t **out_client, const modbus_client_config_t *cfg);
void modbus_client_deinit(modbus_client_t *client);

esp_err_t modbus_read_coils(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint8_t *dest, size_t dest_len);
esp_err_t modbus_read_discrete(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint8_t *dest, size_t dest_len);
esp_err_t modbus_read_holding(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint16_t *dest, size_t dest_len);
esp_err_t modbus_read_input(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t qty, uint16_t *dest, size_t dest_len);
esp_err_t modbus_write_single_coil(modbus_client_t *client, uint8_t slave, uint16_t addr, bool value);
esp_err_t modbus_write_single_register(modbus_client_t *client, uint8_t slave, uint16_t addr, uint16_t value);
esp_err_t modbus_write_multiple_registers(modbus_client_t *client, uint8_t slave, uint16_t addr, const uint16_t *values, uint16_t qty);

uint8_t modbus_client_get_last_exception(const modbus_client_t *client);
