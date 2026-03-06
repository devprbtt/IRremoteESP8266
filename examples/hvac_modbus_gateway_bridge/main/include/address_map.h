#pragma once

#include <stdbool.h>
#include <stdint.h>

uint16_t get_coil_address(uint16_t zone, uint16_t offset);
uint16_t get_discrete_address(uint16_t zone, uint16_t offset);
uint16_t get_holding_address(uint16_t zone, uint16_t offset);
uint16_t get_input_address(uint16_t zone, uint16_t offset);

bool modbus_style_to_protocol(uint32_t style_addr, uint16_t *protocol_addr);