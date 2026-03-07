#include "hvac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "address_map.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telnet_server.h"

#define HVAC_TASK_STACK 6144
#define HVAC_TASK_PRIO 6

static const char *TAG = "hvac";

struct hvac_manager {
    app_config_t cfg;
    modbus_client_t *modbus;
    hvac_zone_state_t zones[APP_CFG_MAX_ZONES];
    uint64_t last_mode_write_ms[APP_CFG_MAX_ZONES];
    SemaphoreHandle_t lock;
    TaskHandle_t poll_task;
    size_t rr_idx;
};

static inline uint16_t midea_discrete_addr(uint16_t n, uint16_t offset1)
{
    return (uint16_t)(n * 8U + (offset1 - 1U)); // 10001+n*8 -> protocol
}

static inline uint16_t midea_input_addr(uint16_t n, uint16_t offset1)
{
    return (uint16_t)(n * 32U + (offset1 - 1U)); // 30001+n*32 -> protocol
}

static inline uint16_t midea_holding_addr(uint16_t n, uint16_t offset1)
{
    return (uint16_t)(n * 25U + offset1); // 40002+n*25 -> protocol offset1=1
}

static inline uint16_t daikin_system_status_addr(void)
{
    return 0;
}

static inline uint16_t daikin_unit_status_addr(uint16_t n)
{
    return (uint16_t)(2000U + (n * 6U));
}

static inline uint16_t daikin_unit_holding_addr(uint16_t n)
{
    return (uint16_t)(2000U + (n * 3U));
}

static inline uint16_t daikin_unit_error_addr(uint16_t n)
{
    return (uint16_t)(3600U + (n * 2U));
}

static inline uint16_t hitachi_holding_addr(uint16_t n, uint16_t offset0)
{
    return (uint16_t)(2000U + (n * 32U) + offset0);
}

static inline uint16_t samsung_holding_addr(uint16_t n, uint16_t offset0)
{
    return (uint16_t)(50U + (n * 50U) + offset0);
}

static inline bool bit_get(const uint8_t *bytes, int bit)
{
    return (bytes[bit / 8] & (1U << (bit % 8))) != 0;
}

static inline bool word_bit_get(const uint16_t *words, size_t word_count, uint16_t bit_pos)
{
    size_t wi = bit_pos / 16U;
    size_t bi = bit_pos % 16U;
    if (wi >= word_count) {
        return false;
    }
    return ((words[wi] >> bi) & 1U) != 0U;
}

static inline uint16_t word_uint_get(const uint16_t *words, size_t word_count, uint16_t start, uint8_t length)
{
    uint16_t out = 0;
    for (uint8_t i = 0; i < length; i++) {
        if (word_bit_get(words, word_count, (uint16_t)(start + i))) {
            out |= (uint16_t)(1U << i);
        }
    }
    return out;
}

static inline void word_uint_set(uint16_t *words, size_t word_count, uint16_t start, uint8_t length, uint16_t value)
{
    for (uint8_t i = 0; i < length; i++) {
        uint16_t bit = (uint16_t)(start + i);
        size_t wi = bit / 16U;
        size_t bi = bit % 16U;
        if (wi >= word_count) {
            continue;
        }
        if ((value >> i) & 1U) {
            words[wi] |= (uint16_t)(1U << bi);
        } else {
            words[wi] &= (uint16_t)~(1U << bi);
        }
    }
}

static inline int16_t daikin_mode_to_generic(uint16_t dm)
{
    switch (dm & 0x0F) {
        case 2:
            return 0; // cool
        case 7:
            return 1; // dry
        case 0:
            return 2; // fan
        case 3:
            return 3; // auto
        case 1:
            return 4; // heat
        default:
            return 3;
    }
}

static inline uint16_t generic_mode_to_daikin(uint16_t gm)
{
    switch (gm) {
        case 0:
            return 2; // cool
        case 1:
            return 7; // dry
        case 2:
            return 0; // fan
        case 3:
            return 3; // auto
        case 4:
            return 1; // heat
        default:
            return 3;
    }
}

static inline uint8_t daikin_fan_to_generic(uint16_t df)
{
    switch (df & 0x07) {
        case 1:
        case 2:
            return 1; // low
        case 3:
        case 4:
            return 2; // mid
        case 5:
            return 3; // high
        case 0:
        default:
            return 4; // auto
    }
}

static inline uint16_t generic_fan_to_daikin(uint16_t gf)
{
    switch (gf) {
        case 1:
            return 1; // low
        case 2:
            return 3; // medium
        case 3:
            return 5; // high
        case 4:
        default:
            return 0; // auto
    }
}

static inline int16_t hitachi_mode_to_generic(uint16_t hm)
{
    switch (hm) {
        case 0:
            return 0; // cool
        case 1:
            return 1; // dry
        case 2:
            return 2; // fan
        case 3:
            return 4; // heat
        case 4:
            return 3; // auto
        default:
            return 3;
    }
}

static inline uint16_t generic_mode_to_hitachi(uint16_t gm)
{
    switch (gm) {
        case 0:
            return 0; // cool
        case 1:
            return 1; // dry
        case 2:
            return 2; // fan
        case 3:
            return 4; // auto
        case 4:
            return 3; // heat
        default:
            return 4;
    }
}

static inline uint8_t hitachi_fan_to_generic(uint16_t hf)
{
    switch (hf) {
        case 0:
            return 1; // low
        case 1:
            return 2; // medium
        case 2:
        case 3:
            return 3; // high/high2
        case 4:
        default:
            return 4; // auto
    }
}

static inline uint16_t generic_fan_to_hitachi(uint16_t gf)
{
    switch (gf) {
        case 1:
            return 0; // low
        case 2:
            return 1; // medium
        case 3:
            return 2; // high
        case 4:
        default:
            return 4; // auto
    }
}

static inline int16_t samsung_mode_to_generic(uint16_t sm)
{
    switch (sm) {
        case 1:
            return 0; // cool
        case 2:
            return 1; // dry
        case 3:
            return 2; // fan
        case 0:
            return 3; // auto
        case 4:
        case 24:
            return 4; // heat / heat storage
        case 21:
            return 0; // cool storage
        default:
            return 3;
    }
}

static inline uint16_t generic_mode_to_samsung(uint16_t gm)
{
    switch (gm) {
        case 0:
            return 1; // cool
        case 1:
            return 2; // dry
        case 2:
            return 3; // fan
        case 3:
            return 0; // auto
        case 4:
            return 4; // heat
        default:
            return 0;
    }
}

static inline uint8_t samsung_fan_to_generic(uint16_t sf)
{
    switch (sf) {
        case 1:
            return 1; // low
        case 2:
            return 2; // middle
        case 3:
            return 3; // high
        case 0:
        default:
            return 4; // auto
    }
}

static inline uint16_t generic_fan_to_samsung(uint16_t gf)
{
    switch (gf) {
        case 1:
            return 1; // low
        case 2:
            return 2; // middle
        case 3:
            return 3; // high
        case 4:
        default:
            return 0; // auto
    }
}

static void set_zone_error(hvac_zone_state_t *z, const char *err)
{
    strlcpy(z->last_error, err, sizeof(z->last_error));
}

static esp_err_t refresh_one(hvac_manager_t *mgr, size_t idx)
{
    hvac_zone_state_t *z = &mgr->zones[idx];
    uint16_t n = z->central_address;

    uint8_t coil_bits[1] = {0};
    uint8_t di_bits[1] = {0};
    uint16_t holding[3] = {0};
    uint16_t input[7] = {0};

    esp_err_t err;

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD) {
        err = modbus_read_discrete(mgr->modbus, z->slave_id, midea_discrete_addr(n, 1), 3, di_bits, sizeof(di_bits));
        if (err != ESP_OK) {
            set_zone_error(z, "midea read discrete failed");
            return err;
        }
        err = modbus_read_input(mgr->modbus, z->slave_id, midea_input_addr(n, 1), 7, input, 7);
        if (err != ESP_OK) {
            set_zone_error(z, "midea read input failed");
            return err;
        }

        z->power = bit_get(di_bits, 0);
        z->alarm = bit_get(di_bits, 1);
        z->connected = bit_get(di_bits, 2);
        z->mode = (uint8_t)(input[0] & 0x1F); // 1 fan,2 cool,3 heat,6 dry
        z->fan_speed = (uint8_t)(input[1] & 0x1F);
        z->setpoint_tenths = (int16_t)input[2];
        z->room_temp_tenths = (int16_t)input[5];
        z->error_code = input[6];
    } else if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_DAIKIN_DTA116A51) {
        uint16_t sys[9] = {0};
        uint16_t status[6] = {0};
        uint16_t error[2] = {0};

        err = modbus_read_input(mgr->modbus, z->slave_id, daikin_system_status_addr(), 9, sys, 9);
        if (err != ESP_OK) {
            set_zone_error(z, "daikin read system failed");
            return err;
        }
        err = modbus_read_input(mgr->modbus, z->slave_id, daikin_unit_status_addr(n), 6, status, 6);
        if (err != ESP_OK) {
            set_zone_error(z, "daikin read status failed");
            return err;
        }
        err = modbus_read_input(mgr->modbus, z->slave_id, daikin_unit_error_addr(n), 2, error, 2);
        if (err != ESP_OK) {
            set_zone_error(z, "daikin read error failed");
            return err;
        }

        z->connected = word_bit_get(sys, 9, (uint16_t)(16U + n));
        z->power = word_bit_get(status, 6, 0);
        z->mode = (uint8_t)daikin_mode_to_generic(word_uint_get(status, 6, 16, 4));
        z->fan_speed = daikin_fan_to_generic(word_uint_get(status, 6, 12, 3));
        z->setpoint_tenths = (int16_t)status[2];
        z->room_temp_tenths = (int16_t)status[4];
        z->alarm = word_bit_get(error, 2, 25);
        z->error_code = (uint16_t)(((error[0] & 0xFFU) << 8) | ((error[0] >> 8) & 0xFFU));
    } else if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_HITACHI_HCA_MB) {
        // Hitachi HC-A(8/16/64)MB indoor unit map:
        // addr = 2000 + (address * 32) + offset
        uint16_t regs[32] = {0};
        err = modbus_read_holding(mgr->modbus, z->slave_id, hitachi_holding_addr(n, 0), 32, regs, 32);
        if (err != ESP_OK) {
            set_zone_error(z, "hitachi read holding failed");
            return err;
        }

        z->connected = (regs[0] != 0U);
        z->power = (regs[9] != 0U);
        z->mode = (uint8_t)hitachi_mode_to_generic(regs[10]);
        z->fan_speed = hitachi_fan_to_generic(regs[11]);
        z->setpoint_tenths = (int16_t)((int16_t)regs[12] * 10);
        z->room_temp_tenths = (int16_t)((int16_t)regs[24] * 10);
        z->error_code = regs[19];
        z->alarm = (regs[22] == 3U) || (regs[19] != 0U);
    } else if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_SAMSUNG_MIM_B19N) {
        // Samsung MIM-B19N/B19NT indoor unit map:
        // addr = 50 + (UI * 50) + offset
        uint16_t regs[32] = {0};
        err = modbus_read_holding(mgr->modbus, z->slave_id, samsung_holding_addr(n, 0), 32, regs, 32);
        if (err != ESP_OK) {
            set_zone_error(z, "samsung read holding failed");
            return err;
        }

        uint16_t comm = regs[0];
        z->connected = ((comm & 0x0001U) != 0U) && ((comm & 0x0004U) != 0U);
        z->power = (regs[2] != 0U);
        z->mode = (uint8_t)samsung_mode_to_generic(regs[3]);
        z->fan_speed = samsung_fan_to_generic(regs[4]);
        z->setpoint_tenths = (int16_t)regs[8];
        z->room_temp_tenths = (int16_t)regs[9];
        z->error_code = regs[14];
        z->alarm = ((comm & 0x0008U) != 0U) || (regs[14] != 0U);
    } else {
        err = modbus_read_coils(mgr->modbus, z->slave_id, get_coil_address(n, 1), 1, coil_bits, sizeof(coil_bits));
        if (err != ESP_OK) {
            set_zone_error(z, "read coil failed");
            return err;
        }

        err = modbus_read_discrete(mgr->modbus, z->slave_id, get_discrete_address(n, 1), 2, di_bits, sizeof(di_bits));
        if (err != ESP_OK) {
            set_zone_error(z, "read discrete failed");
            return err;
        }

        err = modbus_read_holding(mgr->modbus, z->slave_id, get_holding_address(n, 1), 3, holding, 3);
        if (err != ESP_OK) {
            set_zone_error(z, "read holding failed");
            return err;
        }

        err = modbus_read_input(mgr->modbus, z->slave_id, get_input_address(n, 1), 2, input, 2);
        if (err != ESP_OK) {
            set_zone_error(z, "read input failed");
            return err;
        }

        z->power = bit_get(coil_bits, 0);
        z->connected = bit_get(di_bits, 0);
        z->alarm = bit_get(di_bits, 1);
        z->mode = (uint8_t)holding[0];
        z->fan_speed = (uint8_t)holding[1];
        z->setpoint_tenths = (int16_t)holding[2];
        z->error_code = input[0];
        z->room_temp_tenths = (int16_t)input[1];
    }
    z->last_update_ms = (uint64_t)(esp_timer_get_time() / 1000);
    z->last_error[0] = '\0';

    return ESP_OK;
}

static esp_err_t write_verify_power(hvac_manager_t *mgr, hvac_zone_state_t *z, bool value)
{
    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD) {
        uint16_t regs[3] = {0};
        uint16_t n = z->central_address;
        regs[0] = value ? 0xDF : 0x9F; // Midea start/stop with unchanged mode bits
        regs[1] = 0x00FF;              // unchanged fan
        regs[2] = 0x00FF;              // unchanged setpoint
        uint16_t addr = midea_holding_addr(n, 1); // 40002+n*25
        esp_err_t err = modbus_write_multiple_registers(mgr->modbus, z->slave_id, addr, regs, 3);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t bits[1] = {0};
        err = modbus_read_discrete(mgr->modbus, z->slave_id, midea_discrete_addr(n, 1), 1, bits, sizeof(bits));
        if (err != ESP_OK) {
            return err;
        }
        return bit_get(bits, 0) == value ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_DAIKIN_DTA116A51) {
        uint16_t n = z->central_address;
        uint16_t regs[3] = {0};

        regs[0] = value ? 1U : 0U;
        regs[1] = 0;
        regs[2] = (uint16_t)z->setpoint_tenths;
        word_uint_set(regs, 3, 16, 4, generic_mode_to_daikin(z->mode));
        word_uint_set(regs, 3, 12, 3, generic_fan_to_daikin(z->fan_speed));

        uint16_t addr = daikin_unit_holding_addr(n);
        esp_err_t err = modbus_write_multiple_registers(mgr->modbus, z->slave_id, addr, regs, 3);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t verify[6] = {0};
        err = modbus_read_input(mgr->modbus, z->slave_id, daikin_unit_status_addr(n), 6, verify, 6);
        if (err != ESP_OK) {
            return err;
        }
        return (word_bit_get(verify, 6, 0) == value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_HITACHI_HCA_MB) {
        uint16_t addr = hitachi_holding_addr(z->central_address, 3);
        uint16_t raw = value ? 1U : 0U;
        esp_err_t err = modbus_write_single_register(mgr->modbus, z->slave_id, addr, raw);
        if (err != ESP_OK) {
            return err;
        }
        uint16_t verify = 0;
        err = modbus_read_holding(mgr->modbus, z->slave_id, hitachi_holding_addr(z->central_address, 9), 1, &verify, 1);
        if (err != ESP_OK) {
            return err;
        }
        return ((verify != 0U) == value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_SAMSUNG_MIM_B19N) {
        uint16_t addr = samsung_holding_addr(z->central_address, 2);
        uint16_t raw = value ? 1U : 0U;
        esp_err_t err = modbus_write_single_register(mgr->modbus, z->slave_id, addr, raw);
        if (err != ESP_OK) {
            return err;
        }
        uint16_t verify = 0;
        err = modbus_read_holding(mgr->modbus, z->slave_id, addr, 1, &verify, 1);
        if (err != ESP_OK) {
            return err;
        }
        return ((verify != 0U) == value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t addr = get_coil_address(z->central_address, 1);
    esp_err_t err = modbus_write_single_coil(mgr->modbus, z->slave_id, addr, value);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t bits[1] = {0};
    err = modbus_read_coils(mgr->modbus, z->slave_id, addr, 1, bits, sizeof(bits));
    if (err != ESP_OK) {
        return err;
    }
    return bit_get(bits, 0) == value ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t write_verify_holding(hvac_manager_t *mgr, hvac_zone_state_t *z, uint16_t offset, uint16_t value)
{
    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD) {
        uint16_t n = z->central_address;
        uint16_t regs[3] = {0x00FF, 0x00FF, 0x00FF};
        uint16_t read_verify_addr = 0;
        uint16_t read_verify_val = 0;

        if (offset == 1) {
            // mode values exposed as 0 cool,1 dry,2 fan,3 auto,4 heat
            uint16_t mode_code = 2; // cool
            if (value == 1) {
                mode_code = 6; // dry
            } else if (value == 2) {
                mode_code = 1; // fan
            } else if (value == 4) {
                mode_code = 3; // heat
            }
            regs[0] = (uint16_t)(0xC0 | (mode_code & 0x1F)); // power on + mode
            read_verify_addr = midea_input_addr(n, 1);
            read_verify_val = mode_code;
        } else if (offset == 2) {
            // map 1..4 (low/mid/high/auto) to Midea fan code
            uint16_t fan_code = 1;
            if (value == 2) {
                fan_code = 3;
            } else if (value == 3) {
                fan_code = 5;
            } else if (value >= 4) {
                fan_code = 0x80;
            }
            regs[1] = fan_code;
            read_verify_addr = midea_input_addr(n, 2);
            read_verify_val = fan_code;
        } else if (offset == 3) {
            int tenths = (int)value;
            bool half = (tenths % 10) >= 5;
            int whole = tenths / 10;
            if (whole < 1) {
                whole = 1;
            }
            if (whole > 100) {
                whole = 100;
            }
            regs[2] = (uint16_t)((half ? 0x80 : 0x00) | (whole & 0x7F));
            read_verify_addr = midea_input_addr(n, 3);
            read_verify_val = (uint16_t)tenths;
        } else {
            return ESP_ERR_INVALID_ARG;
        }

        uint16_t addr = midea_holding_addr(n, 1);
        esp_err_t err = modbus_write_multiple_registers(mgr->modbus, z->slave_id, addr, regs, 3);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t verify = 0;
        err = modbus_read_input(mgr->modbus, z->slave_id, read_verify_addr, 1, &verify, 1);
        if (err != ESP_OK) {
            return err;
        }
        if (offset == 1) {
            return ((verify & 0x1F) == read_verify_val) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (offset == 2) {
            if (read_verify_val == 0x80) {
                return (verify & 0x80) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
            }
            return ((verify & 0x1F) == (read_verify_val & 0x1F)) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        return (verify == read_verify_val) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_DAIKIN_DTA116A51) {
        uint16_t n = z->central_address;
        uint16_t regs[3] = {0};

        regs[0] = z->power ? 1U : 0U;
        regs[1] = 0;
        regs[2] = (uint16_t)z->setpoint_tenths;

        uint16_t want_mode = generic_mode_to_daikin(z->mode);
        uint16_t want_fan = generic_fan_to_daikin(z->fan_speed);

        if (offset == 1) {
            want_mode = generic_mode_to_daikin(value);
            regs[0] = 1U;
        } else if (offset == 2) {
            want_fan = generic_fan_to_daikin(value);
        } else if (offset == 3) {
            regs[2] = value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }

        word_uint_set(regs, 3, 16, 4, want_mode);
        word_uint_set(regs, 3, 12, 3, want_fan);

        uint16_t addr = daikin_unit_holding_addr(n);
        esp_err_t err = modbus_write_multiple_registers(mgr->modbus, z->slave_id, addr, regs, 3);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t verify[6] = {0};
        err = modbus_read_input(mgr->modbus, z->slave_id, daikin_unit_status_addr(n), 6, verify, 6);
        if (err != ESP_OK) {
            return err;
        }

        if (offset == 1) {
            return (daikin_mode_to_generic(word_uint_get(verify, 6, 16, 4)) == (int16_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (offset == 2) {
            return (daikin_fan_to_generic(word_uint_get(verify, 6, 12, 3)) == (uint8_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        return ((int16_t)verify[2] == (int16_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_HITACHI_HCA_MB) {
        uint16_t n = z->central_address;
        uint16_t write_offset = 0;
        uint16_t verify_offset = 0;
        uint16_t raw = 0;

        if (offset == 1) {
            write_offset = 4;  // mode order
            verify_offset = 10; // mode status
            raw = generic_mode_to_hitachi(value);
        } else if (offset == 2) {
            write_offset = 5;  // fan order
            verify_offset = 11; // fan status
            raw = generic_fan_to_hitachi(value);
        } else if (offset == 3) {
            write_offset = 6;  // setpoint order (integer C)
            verify_offset = 12; // setpoint status (integer C)
            int32_t whole = ((int32_t)value + 5) / 10;
            if (whole < 0) {
                whole = 0;
            } else if (whole > 50) {
                whole = 50;
            }
            raw = (uint16_t)whole;
        } else {
            return ESP_ERR_INVALID_ARG;
        }

        uint16_t addr = hitachi_holding_addr(n, write_offset);
        esp_err_t err = modbus_write_single_register(mgr->modbus, z->slave_id, addr, raw);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t verify = 0;
        err = modbus_read_holding(mgr->modbus, z->slave_id, hitachi_holding_addr(n, verify_offset), 1, &verify, 1);
        if (err != ESP_OK) {
            return err;
        }

        if (offset == 1) {
            return (hitachi_mode_to_generic(verify) == (int16_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (offset == 2) {
            return (hitachi_fan_to_generic(verify) == (uint8_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        int16_t verify_tenths = (int16_t)((int16_t)verify * 10);
        return (verify_tenths == (int16_t)(((int32_t)value + 5) / 10 * 10)) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_SAMSUNG_MIM_B19N) {
        uint16_t n = z->central_address;
        uint16_t write_offset = 0;
        uint16_t raw = 0;

        if (offset == 1) {
            write_offset = 3;
            raw = generic_mode_to_samsung(value);
        } else if (offset == 2) {
            write_offset = 4;
            raw = generic_fan_to_samsung(value);
        } else if (offset == 3) {
            write_offset = 8;
            raw = value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }

        uint16_t addr = samsung_holding_addr(n, write_offset);
        esp_err_t err = modbus_write_single_register(mgr->modbus, z->slave_id, addr, raw);
        if (err != ESP_OK) {
            return err;
        }

        uint16_t verify = 0;
        err = modbus_read_holding(mgr->modbus, z->slave_id, addr, 1, &verify, 1);
        if (err != ESP_OK) {
            return err;
        }

        if (offset == 1) {
            return (samsung_mode_to_generic(verify) == (int16_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        if (offset == 2) {
            return (samsung_fan_to_generic(verify) == (uint8_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        return ((int16_t)verify == (int16_t)value) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t addr = get_holding_address(z->central_address, offset);
    esp_err_t err = modbus_write_single_register(mgr->modbus, z->slave_id, addr, value);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t reg = 0;
    err = modbus_read_holding(mgr->modbus, z->slave_id, addr, 1, &reg, 1);
    if (err != ESP_OK) {
        return err;
    }
    return reg == value ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void poll_task_fn(void *arg)
{
    hvac_manager_t *mgr = (hvac_manager_t *)arg;

    while (1) {
        size_t idx = mgr->rr_idx++ % mgr->cfg.hvac.zone_count;

        if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            esp_err_t err = refresh_one(mgr, idx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "poll zone idx=%u ca=%u slave=%u failed: 0x%x",
                         (unsigned)idx,
                         mgr->zones[idx].central_address,
                         mgr->zones[idx].slave_id,
                         err);
                telnet_server_logf("poll zone idx=%u ca=%u slave=%u failed: 0x%x",
                                   (unsigned)idx,
                                   mgr->zones[idx].central_address,
                                   mgr->zones[idx].slave_id,
                                   err);
            }
            xSemaphoreGive(mgr->lock);
        }

        vTaskDelay(pdMS_TO_TICKS(mgr->cfg.hvac.poll_interval_ms));
    }
}

esp_err_t hvac_manager_init(hvac_manager_t **out_mgr, const app_config_t *cfg, modbus_client_t *modbus)
{
    if (!out_mgr || !cfg || !modbus) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->hvac.zone_count == 0 || cfg->hvac.zone_count > APP_CFG_MAX_ZONES) {
        return ESP_ERR_INVALID_ARG;
    }

    hvac_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) {
        return ESP_ERR_NO_MEM;
    }

    mgr->cfg = *cfg;
    mgr->modbus = modbus;
    mgr->lock = xSemaphoreCreateMutex();
    if (!mgr->lock) {
        free(mgr);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < cfg->hvac.zone_count; i++) {
        const zone_cfg_t *z = &cfg->hvac.zones[i];
        if ((int)z->central_address < cfg->hvac.idu_address_base) {
            ESP_LOGE(TAG, "zone central address %u is below idu_address_base=%d", z->central_address, cfg->hvac.idu_address_base);
            vSemaphoreDelete(mgr->lock);
            free(mgr);
            return ESP_ERR_INVALID_ARG;
        }

        mgr->zones[i].central_address = z->central_address;
        mgr->zones[i].slave_id = z->slave_id;
        set_zone_error(&mgr->zones[i], "not polled yet");
    }

    *out_mgr = mgr;
    return ESP_OK;
}

void hvac_manager_start(hvac_manager_t *mgr)
{
    if (!mgr || mgr->poll_task) {
        return;
    }
    xTaskCreate(poll_task_fn, "hvac_poll", HVAC_TASK_STACK, mgr, HVAC_TASK_PRIO, &mgr->poll_task);
}

void hvac_manager_deinit(hvac_manager_t *mgr)
{
    if (!mgr) {
        return;
    }
    if (mgr->poll_task) {
        vTaskDelete(mgr->poll_task);
    }
    if (mgr->lock) {
        vSemaphoreDelete(mgr->lock);
    }
    free(mgr);
}

size_t hvac_manager_zone_count(const hvac_manager_t *mgr)
{
    return mgr ? mgr->cfg.hvac.zone_count : 0;
}

const hvac_zone_state_t *hvac_manager_get_zone(const hvac_manager_t *mgr, size_t index)
{
    if (!mgr || index >= mgr->cfg.hvac.zone_count) {
        return NULL;
    }
    return &mgr->zones[index];
}

esp_err_t hvac_manager_refresh_zone(hvac_manager_t *mgr, size_t zone_index)
{
    if (!mgr || zone_index >= mgr->cfg.hvac.zone_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = refresh_one(mgr, zone_index);
    xSemaphoreGive(mgr->lock);
    return err;
}

esp_err_t hvac_manager_set_field(hvac_manager_t *mgr, size_t zone_index, const char *field, const char *value, char *msg, size_t msg_len)
{
    if (!mgr || !field || !value || !msg || msg_len == 0 || zone_index >= mgr->cfg.hvac.zone_count) {
        return ESP_ERR_INVALID_ARG;
    }

    hvac_zone_state_t *z = &mgr->zones[zone_index];
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(4000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!z->connected) {
        snprintf(msg, msg_len, "zone offline");
        telnet_server_logf("set zone=%u rejected=offline", (unsigned)zone_index);
        xSemaphoreGive(mgr->lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (strcasecmp(field, "power") == 0) {
        long v = strtol(value, NULL, 10);
        bool b = (v != 0);
        err = write_verify_power(mgr, z, b);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            err = write_verify_power(mgr, z, b);
        }
    } else if (strcasecmp(field, "mode") == 0) {
        long v = strtol(value, NULL, 10);
        if (v < 0 || v > 4) {
            err = ESP_ERR_INVALID_ARG;
        } else if (mgr->cfg.hvac.gateway_type == HVAC_GATEWAY_MIDEA_GW3_MOD && v == 3) {
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
            uint64_t elapsed = now_ms - mgr->last_mode_write_ms[zone_index];
            if (elapsed < (uint64_t)mgr->cfg.hvac.mode_rate_limit_ms) {
                err = ESP_ERR_INVALID_STATE;
            } else {
                err = write_verify_holding(mgr, z, 1, (uint16_t)v);
                if (err != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    err = write_verify_holding(mgr, z, 1, (uint16_t)v);
                }
                if (err == ESP_OK) {
                    mgr->last_mode_write_ms[zone_index] = now_ms;
                }
            }
        }
    } else if (strcasecmp(field, "fan") == 0 || strcasecmp(field, "fan_speed") == 0) {
        long v = strtol(value, NULL, 10);
        if (v < 1 || v > 4) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = write_verify_holding(mgr, z, 2, (uint16_t)v);
            if (err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(100));
                err = write_verify_holding(mgr, z, 2, (uint16_t)v);
            }
        }
    } else if (strcasecmp(field, "setpoint") == 0) {
        float temp_c = strtof(value, NULL);
        int tenths = (int)(temp_c * 10.0f + (temp_c >= 0 ? 0.5f : -0.5f));
        if (tenths < mgr->cfg.hvac.setpoint_min_tenths) {
            tenths = mgr->cfg.hvac.setpoint_min_tenths;
        }
        if (tenths > mgr->cfg.hvac.setpoint_max_tenths) {
            tenths = mgr->cfg.hvac.setpoint_max_tenths;
        }
        err = write_verify_holding(mgr, z, 3, (uint16_t)tenths);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            err = write_verify_holding(mgr, z, 3, (uint16_t)tenths);
        }
    }

    if (err == ESP_OK) {
        refresh_one(mgr, zone_index);
        snprintf(msg, msg_len, "ok");
        telnet_server_logf("set zone=%u field=%s value=%s result=ok",
                           (unsigned)zone_index,
                           field,
                           value);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        snprintf(msg, msg_len, "value not supported for gateway");
        telnet_server_logf("set zone=%u field=%s value=%s rejected=not_supported",
                           (unsigned)zone_index,
                           field,
                           value);
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(msg, msg_len, "mode rate-limited");
        telnet_server_logf("set zone=%u field=%s blocked=rate_limited",
                           (unsigned)zone_index,
                           field);
    } else {
        snprintf(msg, msg_len, "write failed: 0x%x", err);
        telnet_server_logf("set zone=%u field=%s value=%s failed=0x%x",
                           (unsigned)zone_index,
                           field,
                           value,
                           err);
    }

    xSemaphoreGive(mgr->lock);
    return err;
}

esp_err_t hvac_manager_zone_to_json(const hvac_manager_t *mgr, size_t zone_index, char *out, size_t out_len)
{
    if (!mgr || !out || out_len == 0 || zone_index >= mgr->cfg.hvac.zone_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const hvac_zone_state_t *z = &mgr->zones[zone_index];
    int w = snprintf(out,
                     out_len,
                     "{\"index\":%u,\"slave\":%u,\"central_address\":%u,\"power\":%s,\"mode\":%u,\"fan_speed\":%u,\"setpoint_c\":%.1f,\"room_temperature_c\":%.1f,\"connected\":%s,\"alarm\":%s,\"error_code\":%u,\"last_update_ms\":%llu,\"last_error\":\"%s\"}",
                     (unsigned)zone_index,
                     z->slave_id,
                     z->central_address,
                     z->power ? "true" : "false",
                     z->mode,
                     z->fan_speed,
                     z->setpoint_tenths / 10.0f,
                     z->room_temp_tenths / 10.0f,
                     z->connected ? "true" : "false",
                     z->alarm ? "true" : "false",
                     z->error_code,
                     (unsigned long long)z->last_update_ms,
                     z->last_error);
    return (w < 0 || (size_t)w >= out_len) ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t hvac_manager_zones_to_json(const hvac_manager_t *mgr, char *out, size_t out_len)
{
    if (!mgr || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;
    int n = snprintf(out, out_len, "[");
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_NO_MEM;
    }
    off = (size_t)n;

    char one[384];
    for (size_t i = 0; i < mgr->cfg.hvac.zone_count; i++) {
        if (hvac_manager_zone_to_json(mgr, i, one, sizeof(one)) != ESP_OK) {
            return ESP_FAIL;
        }

        n = snprintf(out + off, out_len - off, "%s%s", (i == 0) ? "" : ",", one);
        if (n < 0 || (size_t)n >= out_len - off) {
            return ESP_ERR_NO_MEM;
        }
        off += (size_t)n;
    }

    n = snprintf(out + off, out_len - off, "]");
    if (n < 0 || (size_t)n >= out_len - off) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
