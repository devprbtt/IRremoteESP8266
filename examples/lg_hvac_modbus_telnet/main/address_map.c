#include "address_map.h"

uint16_t get_coil_address(uint16_t zone, uint16_t offset)
{
    return (uint16_t)(zone * 16U + offset);
}

uint16_t get_discrete_address(uint16_t zone, uint16_t offset)
{
    return (uint16_t)(zone * 16U + offset);
}

uint16_t get_holding_address(uint16_t zone, uint16_t offset)
{
    return (uint16_t)(zone * 20U + offset);
}

uint16_t get_input_address(uint16_t zone, uint16_t offset)
{
    return (uint16_t)(zone * 20U + offset);
}

bool modbus_style_to_protocol(uint32_t style_addr, uint16_t *protocol_addr)
{
    if (!protocol_addr) {
        return false;
    }

    if (style_addr >= 40001 && style_addr <= 49999) {
        *protocol_addr = (uint16_t)(style_addr - 40001);
        return true;
    }
    if (style_addr >= 30001 && style_addr <= 39999) {
        *protocol_addr = (uint16_t)(style_addr - 30001);
        return true;
    }
    if (style_addr >= 10001 && style_addr <= 19999) {
        *protocol_addr = (uint16_t)(style_addr - 10001);
        return true;
    }
    if (style_addr >= 1 && style_addr <= 9999) {
        *protocol_addr = (uint16_t)(style_addr - 1);
        return true;
    }

    return false;
}