#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    D3NET_REG_INPUT = 0,
    D3NET_REG_HOLDING = 1,
} d3net_reg_type_t;

typedef enum {
    D3NET_MODE_FAN = 0,
    D3NET_MODE_HEAT = 1,
    D3NET_MODE_COOL = 2,
    D3NET_MODE_AUTO = 3,
    D3NET_MODE_VENT = 4,
    D3NET_MODE_UNDEFINED = 5,
    D3NET_MODE_SLAVE = 6,
    D3NET_MODE_DRY = 7,
} d3net_mode_t;

typedef enum {
    D3NET_FAN_SPEED_AUTO = 0,
    D3NET_FAN_SPEED_LOW = 1,
    D3NET_FAN_SPEED_LOW_MEDIUM = 2,
    D3NET_FAN_SPEED_MEDIUM = 3,
    D3NET_FAN_SPEED_HIGH_MEDIUM = 4,
    D3NET_FAN_SPEED_HIGH = 5,
} d3net_fan_speed_t;

typedef enum {
    D3NET_FAN_DIR_P0 = 0,
    D3NET_FAN_DIR_P1 = 1,
    D3NET_FAN_DIR_P2 = 2,
    D3NET_FAN_DIR_P3 = 3,
    D3NET_FAN_DIR_P4 = 4,
    D3NET_FAN_DIR_STOP = 6,
    D3NET_FAN_DIR_SWING = 7,
} d3net_fan_dir_t;

typedef struct {
    uint16_t words[9];
} d3net_system_status_t;

typedef struct {
    uint16_t words[3];
} d3net_unit_cap_t;

typedef struct {
    uint16_t words[6];
} d3net_unit_status_t;

typedef struct {
    uint16_t words[3];
    bool dirty;
    uint64_t last_read_ms;
    uint64_t last_write_ms;
} d3net_unit_holding_t;

typedef struct {
    uint16_t words[2];
} d3net_unit_error_t;

enum {
    D3NET_ADDR_SYSTEM_STATUS = 0,
    D3NET_CNT_SYSTEM_STATUS = 9,
    D3NET_ADDR_UNIT_CAP = 1000,
    D3NET_CNT_UNIT_CAP = 3,
    D3NET_ADDR_UNIT_STATUS = 2000,
    D3NET_CNT_UNIT_STATUS = 6,
    D3NET_ADDR_UNIT_HOLDING = 2000,
    D3NET_CNT_UNIT_HOLDING = 3,
    D3NET_ADDR_UNIT_ERROR = 3600,
    D3NET_CNT_UNIT_ERROR = 2,
};

bool d3net_bit_get(const uint16_t *words, size_t word_count, uint16_t bit_pos);
void d3net_bit_set(uint16_t *words, size_t word_count, uint16_t bit_pos, bool value, bool *dirty);
uint32_t d3net_uint_get(const uint16_t *words, size_t word_count, uint16_t start, uint8_t length);
void d3net_uint_set(uint16_t *words, size_t word_count, uint16_t start, uint8_t length, uint32_t value, bool *dirty);
int32_t d3net_sint_get(const uint16_t *words, size_t word_count, uint16_t start, uint8_t length);
void d3net_sint_set(uint16_t *words, size_t word_count, uint16_t start, uint8_t length, int32_t value, bool *dirty);

bool d3net_system_initialized(const d3net_system_status_t *s);
bool d3net_system_other_device_exists(const d3net_system_status_t *s);
bool d3net_system_unit_connected(const d3net_system_status_t *s, uint8_t unit_index);
bool d3net_system_unit_error(const d3net_system_status_t *s, uint8_t unit_index);

bool d3net_cap_mode_fan(const d3net_unit_cap_t *c);
bool d3net_cap_mode_cool(const d3net_unit_cap_t *c);
bool d3net_cap_mode_heat(const d3net_unit_cap_t *c);
bool d3net_cap_mode_auto(const d3net_unit_cap_t *c);
bool d3net_cap_mode_dry(const d3net_unit_cap_t *c);
bool d3net_cap_fan_speed(const d3net_unit_cap_t *c);
bool d3net_cap_fan_dir(const d3net_unit_cap_t *c);
uint8_t d3net_cap_fan_speed_steps(const d3net_unit_cap_t *c);
uint8_t d3net_cap_fan_dir_steps(const d3net_unit_cap_t *c);
int8_t d3net_cap_cool_setpoint_upper(const d3net_unit_cap_t *c);
int8_t d3net_cap_cool_setpoint_lower(const d3net_unit_cap_t *c);
int8_t d3net_cap_heat_setpoint_upper(const d3net_unit_cap_t *c);
int8_t d3net_cap_heat_setpoint_lower(const d3net_unit_cap_t *c);

bool d3net_status_power_get(const d3net_unit_status_t *s);
void d3net_status_power_set(d3net_unit_status_t *s, bool value);
d3net_mode_t d3net_status_oper_mode_get(const d3net_unit_status_t *s);
void d3net_status_oper_mode_set(d3net_unit_status_t *s, d3net_mode_t mode);
d3net_mode_t d3net_status_oper_current_get(const d3net_unit_status_t *s);
d3net_fan_speed_t d3net_status_fan_speed_get(const d3net_unit_status_t *s);
void d3net_status_fan_speed_set(d3net_unit_status_t *s, d3net_fan_speed_t speed);
d3net_fan_dir_t d3net_status_fan_dir_get(const d3net_unit_status_t *s);
void d3net_status_fan_dir_set(d3net_unit_status_t *s, d3net_fan_dir_t dir);
bool d3net_status_filter_warning_get(const d3net_unit_status_t *s);
float d3net_status_temp_setpoint_get(const d3net_unit_status_t *s);
void d3net_status_temp_setpoint_set(d3net_unit_status_t *s, float value_c);
float d3net_status_temp_current_get(const d3net_unit_status_t *s);

bool d3net_holding_power_get(const d3net_unit_holding_t *h);
void d3net_holding_power_set(d3net_unit_holding_t *h, bool value);
d3net_mode_t d3net_holding_oper_mode_get(const d3net_unit_holding_t *h);
void d3net_holding_oper_mode_set(d3net_unit_holding_t *h, d3net_mode_t mode);
float d3net_holding_temp_setpoint_get(const d3net_unit_holding_t *h);
void d3net_holding_temp_setpoint_set(d3net_unit_holding_t *h, float value_c);
d3net_fan_speed_t d3net_holding_fan_speed_get(const d3net_unit_holding_t *h);
void d3net_holding_fan_speed_set(d3net_unit_holding_t *h, d3net_fan_speed_t speed);
d3net_fan_dir_t d3net_holding_fan_dir_get(const d3net_unit_holding_t *h);
void d3net_holding_fan_dir_set(d3net_unit_holding_t *h, d3net_fan_dir_t dir);
bool d3net_holding_filter_reset_get(const d3net_unit_holding_t *h);
void d3net_holding_filter_reset_set(d3net_unit_holding_t *h, bool value);
bool d3net_holding_fan_control_get(const d3net_unit_holding_t *h);
void d3net_holding_fan_control_set(d3net_unit_holding_t *h, bool enabled);
void d3net_holding_mark_read(d3net_unit_holding_t *h, uint64_t now_ms);
void d3net_holding_mark_written(d3net_unit_holding_t *h, uint64_t now_ms);
bool d3net_holding_read_within(const d3net_unit_holding_t *h, uint64_t now_ms, uint32_t sec);
bool d3net_holding_write_within(const d3net_unit_holding_t *h, uint64_t now_ms, uint32_t sec);

void d3net_holding_sync_from_status(d3net_unit_holding_t *h, const d3net_unit_status_t *s);

char d3net_error_code_char0(const d3net_unit_error_t *e);
char d3net_error_code_char1(const d3net_unit_error_t *e);
uint8_t d3net_error_subcode(const d3net_unit_error_t *e);
bool d3net_error_error(const d3net_unit_error_t *e);
bool d3net_error_alarm(const d3net_unit_error_t *e);
bool d3net_error_warning(const d3net_unit_error_t *e);
uint8_t d3net_error_unit_number(const d3net_unit_error_t *e);

#ifdef __cplusplus
}
#endif
