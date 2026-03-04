#include "d3net_codec.h"

#include <math.h>
#include <string.h>

static inline bool d3net_valid_bit(size_t word_count, uint16_t bit_pos) {
    return (bit_pos / 16U) < word_count;
}

bool d3net_bit_get(const uint16_t *words, size_t word_count, uint16_t bit_pos) {
    if (!d3net_valid_bit(word_count, bit_pos)) {
        return false;
    }
    const uint16_t word = words[bit_pos / 16U];
    const uint16_t mask = (uint16_t)(1U << (bit_pos % 16U));
    return (word & mask) != 0U;
}

void d3net_bit_set(uint16_t *words, size_t word_count, uint16_t bit_pos, bool value, bool *dirty) {
    if (!d3net_valid_bit(word_count, bit_pos)) {
        return;
    }
    const uint16_t idx = (uint16_t)(bit_pos / 16U);
    const uint16_t mask = (uint16_t)(1U << (bit_pos % 16U));
    const bool current = (words[idx] & mask) != 0U;
    if (current == value) {
        return;
    }
    if (value) {
        words[idx] |= mask;
    } else {
        words[idx] &= (uint16_t)(~mask);
    }
    if (dirty != NULL) {
        *dirty = true;
    }
}

uint32_t d3net_uint_get(const uint16_t *words, size_t word_count, uint16_t start, uint8_t length) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < length; i++) {
        if (d3net_bit_get(words, word_count, (uint16_t)(start + i))) {
            value |= (uint32_t)(1UL << i);
        }
    }
    return value;
}

void d3net_uint_set(uint16_t *words, size_t word_count, uint16_t start, uint8_t length, uint32_t value, bool *dirty) {
    for (uint8_t i = 0; i < length; i++) {
        const bool bit = (value & (1UL << i)) != 0U;
        d3net_bit_set(words, word_count, (uint16_t)(start + i), bit, dirty);
    }
}

int32_t d3net_sint_get(const uint16_t *words, size_t word_count, uint16_t start, uint8_t length) {
    if (length < 2) {
        return 0;
    }
    int32_t value = (int32_t)d3net_uint_get(words, word_count, start, (uint8_t)(length - 1U));
    if (d3net_bit_get(words, word_count, (uint16_t)(start + length - 1U))) {
        value = -value;
    }
    return value;
}

void d3net_sint_set(uint16_t *words, size_t word_count, uint16_t start, uint8_t length, int32_t value, bool *dirty) {
    if (length < 2) {
        return;
    }
    const bool sign = value < 0;
    const uint32_t mag = (uint32_t)(sign ? -value : value);
    d3net_uint_set(words, word_count, start, (uint8_t)(length - 1U), mag, dirty);
    d3net_bit_set(words, word_count, (uint16_t)(start + length - 1U), sign, dirty);
}

bool d3net_system_initialized(const d3net_system_status_t *s) {
    return d3net_bit_get(s->words, D3NET_CNT_SYSTEM_STATUS, 0);
}

bool d3net_system_other_device_exists(const d3net_system_status_t *s) {
    return d3net_bit_get(s->words, D3NET_CNT_SYSTEM_STATUS, 1);
}

bool d3net_system_unit_connected(const d3net_system_status_t *s, uint8_t unit_index) {
    if (unit_index >= 64U) {
        return false;
    }
    return d3net_bit_get(s->words, D3NET_CNT_SYSTEM_STATUS, (uint16_t)(16U + unit_index));
}

bool d3net_system_unit_error(const d3net_system_status_t *s, uint8_t unit_index) {
    if (unit_index >= 64U) {
        return false;
    }
    return d3net_bit_get(s->words, D3NET_CNT_SYSTEM_STATUS, (uint16_t)(80U + unit_index));
}

bool d3net_cap_mode_fan(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 0);
}

bool d3net_cap_mode_cool(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 1);
}

bool d3net_cap_mode_heat(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 2);
}

bool d3net_cap_mode_auto(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 3);
}

bool d3net_cap_mode_dry(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 4);
}

bool d3net_cap_fan_speed(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 15);
}

bool d3net_cap_fan_dir(const d3net_unit_cap_t *c) {
    return d3net_bit_get(c->words, D3NET_CNT_UNIT_CAP, 11);
}

uint8_t d3net_cap_fan_speed_steps(const d3net_unit_cap_t *c) {
    return (uint8_t)d3net_uint_get(c->words, D3NET_CNT_UNIT_CAP, 12, 3);
}

uint8_t d3net_cap_fan_dir_steps(const d3net_unit_cap_t *c) {
    return (uint8_t)d3net_uint_get(c->words, D3NET_CNT_UNIT_CAP, 8, 3);
}

int8_t d3net_cap_cool_setpoint_upper(const d3net_unit_cap_t *c) {
    return (int8_t)d3net_sint_get(c->words, D3NET_CNT_UNIT_CAP, 16, 8);
}

int8_t d3net_cap_cool_setpoint_lower(const d3net_unit_cap_t *c) {
    return (int8_t)d3net_sint_get(c->words, D3NET_CNT_UNIT_CAP, 24, 8);
}

int8_t d3net_cap_heat_setpoint_upper(const d3net_unit_cap_t *c) {
    return (int8_t)d3net_sint_get(c->words, D3NET_CNT_UNIT_CAP, 32, 8);
}

int8_t d3net_cap_heat_setpoint_lower(const d3net_unit_cap_t *c) {
    return (int8_t)d3net_sint_get(c->words, D3NET_CNT_UNIT_CAP, 40, 8);
}

bool d3net_status_power_get(const d3net_unit_status_t *s) {
    return d3net_bit_get(s->words, D3NET_CNT_UNIT_STATUS, 0);
}

void d3net_status_power_set(d3net_unit_status_t *s, bool value) {
    bool dirty = false;
    d3net_bit_set(s->words, D3NET_CNT_UNIT_STATUS, 0, value, &dirty);
    (void)dirty;
}

d3net_mode_t d3net_status_oper_mode_get(const d3net_unit_status_t *s) {
    return (d3net_mode_t)d3net_uint_get(s->words, D3NET_CNT_UNIT_STATUS, 16, 4);
}

void d3net_status_oper_mode_set(d3net_unit_status_t *s, d3net_mode_t mode) {
    bool dirty = false;
    d3net_uint_set(s->words, D3NET_CNT_UNIT_STATUS, 16, 4, (uint32_t)mode, &dirty);
    (void)dirty;
}

d3net_mode_t d3net_status_oper_current_get(const d3net_unit_status_t *s) {
    return (d3net_mode_t)d3net_uint_get(s->words, D3NET_CNT_UNIT_STATUS, 24, 4);
}

d3net_fan_speed_t d3net_status_fan_speed_get(const d3net_unit_status_t *s) {
    return (d3net_fan_speed_t)d3net_uint_get(s->words, D3NET_CNT_UNIT_STATUS, 12, 3);
}

void d3net_status_fan_speed_set(d3net_unit_status_t *s, d3net_fan_speed_t speed) {
    bool dirty = false;
    d3net_uint_set(s->words, D3NET_CNT_UNIT_STATUS, 12, 3, (uint32_t)speed, &dirty);
    (void)dirty;
}

d3net_fan_dir_t d3net_status_fan_dir_get(const d3net_unit_status_t *s) {
    return (d3net_fan_dir_t)d3net_uint_get(s->words, D3NET_CNT_UNIT_STATUS, 8, 3);
}

void d3net_status_fan_dir_set(d3net_unit_status_t *s, d3net_fan_dir_t dir) {
    bool dirty = false;
    d3net_uint_set(s->words, D3NET_CNT_UNIT_STATUS, 8, 3, (uint32_t)dir, &dirty);
    (void)dirty;
}

bool d3net_status_filter_warning_get(const d3net_unit_status_t *s) {
    return d3net_uint_get(s->words, D3NET_CNT_UNIT_STATUS, 20, 4) != 0U;
}

float d3net_status_temp_setpoint_get(const d3net_unit_status_t *s) {
    const int32_t x10 = d3net_sint_get(s->words, D3NET_CNT_UNIT_STATUS, 32, 16);
    return (float)x10 / 10.0f;
}

void d3net_status_temp_setpoint_set(d3net_unit_status_t *s, float value_c) {
    const int32_t x10 = (int32_t)llroundf(value_c * 10.0f);
    bool dirty = false;
    d3net_sint_set(s->words, D3NET_CNT_UNIT_STATUS, 32, 16, x10, &dirty);
    (void)dirty;
}

float d3net_status_temp_current_get(const d3net_unit_status_t *s) {
    const int32_t x10 = d3net_sint_get(s->words, D3NET_CNT_UNIT_STATUS, 64, 16);
    return (float)x10 / 10.0f;
}

bool d3net_holding_power_get(const d3net_unit_holding_t *h) {
    return d3net_bit_get(h->words, D3NET_CNT_UNIT_HOLDING, 0);
}

void d3net_holding_power_set(d3net_unit_holding_t *h, bool value) {
    d3net_bit_set(h->words, D3NET_CNT_UNIT_HOLDING, 0, value, &h->dirty);
}

d3net_mode_t d3net_holding_oper_mode_get(const d3net_unit_holding_t *h) {
    return (d3net_mode_t)d3net_uint_get(h->words, D3NET_CNT_UNIT_HOLDING, 16, 4);
}

void d3net_holding_oper_mode_set(d3net_unit_holding_t *h, d3net_mode_t mode) {
    d3net_uint_set(h->words, D3NET_CNT_UNIT_HOLDING, 16, 4, (uint32_t)mode, &h->dirty);
}

float d3net_holding_temp_setpoint_get(const d3net_unit_holding_t *h) {
    const int32_t x10 = d3net_sint_get(h->words, D3NET_CNT_UNIT_HOLDING, 32, 16);
    return (float)x10 / 10.0f;
}

void d3net_holding_temp_setpoint_set(d3net_unit_holding_t *h, float value_c) {
    const int32_t x10 = (int32_t)llroundf(value_c * 10.0f);
    d3net_sint_set(h->words, D3NET_CNT_UNIT_HOLDING, 32, 16, x10, &h->dirty);
}

d3net_fan_speed_t d3net_holding_fan_speed_get(const d3net_unit_holding_t *h) {
    return (d3net_fan_speed_t)d3net_uint_get(h->words, D3NET_CNT_UNIT_HOLDING, 12, 3);
}

void d3net_holding_fan_speed_set(d3net_unit_holding_t *h, d3net_fan_speed_t speed) {
    d3net_uint_set(h->words, D3NET_CNT_UNIT_HOLDING, 12, 3, (uint32_t)speed, &h->dirty);
    d3net_holding_fan_control_set(h, true);
}

d3net_fan_dir_t d3net_holding_fan_dir_get(const d3net_unit_holding_t *h) {
    return (d3net_fan_dir_t)d3net_uint_get(h->words, D3NET_CNT_UNIT_HOLDING, 8, 3);
}

void d3net_holding_fan_dir_set(d3net_unit_holding_t *h, d3net_fan_dir_t dir) {
    d3net_uint_set(h->words, D3NET_CNT_UNIT_HOLDING, 8, 3, (uint32_t)dir, &h->dirty);
    d3net_holding_fan_control_set(h, true);
}

bool d3net_holding_filter_reset_get(const d3net_unit_holding_t *h) {
    return d3net_uint_get(h->words, D3NET_CNT_UNIT_HOLDING, 20, 4) != 0U;
}

void d3net_holding_filter_reset_set(d3net_unit_holding_t *h, bool value) {
    d3net_uint_set(h->words, D3NET_CNT_UNIT_HOLDING, 20, 4, value ? 15U : 0U, &h->dirty);
}

bool d3net_holding_fan_control_get(const d3net_unit_holding_t *h) {
    return d3net_uint_get(h->words, D3NET_CNT_UNIT_HOLDING, 4, 4) == 6U;
}

void d3net_holding_fan_control_set(d3net_unit_holding_t *h, bool enabled) {
    d3net_uint_set(h->words, D3NET_CNT_UNIT_HOLDING, 4, 4, enabled ? 6U : 0U, &h->dirty);
}

void d3net_holding_mark_read(d3net_unit_holding_t *h, uint64_t now_ms) {
    h->last_read_ms = now_ms;
}

void d3net_holding_mark_written(d3net_unit_holding_t *h, uint64_t now_ms) {
    h->last_write_ms = now_ms;
    h->dirty = false;
}

bool d3net_holding_read_within(const d3net_unit_holding_t *h, uint64_t now_ms, uint32_t sec) {
    if (h->last_read_ms == 0U) {
        return false;
    }
    return (now_ms - h->last_read_ms) < ((uint64_t)sec * 1000ULL);
}

bool d3net_holding_write_within(const d3net_unit_holding_t *h, uint64_t now_ms, uint32_t sec) {
    if (h->last_write_ms == 0U) {
        return false;
    }
    return (now_ms - h->last_write_ms) < ((uint64_t)sec * 1000ULL);
}

void d3net_holding_sync_from_status(d3net_unit_holding_t *h, const d3net_unit_status_t *s) {
    d3net_holding_power_set(h, d3net_status_power_get(s));
    d3net_holding_fan_dir_set(h, d3net_status_fan_dir_get(s));
    d3net_holding_fan_speed_set(h, d3net_status_fan_speed_get(s));
    d3net_holding_oper_mode_set(h, d3net_status_oper_mode_get(s));
    d3net_holding_temp_setpoint_set(h, d3net_status_temp_setpoint_get(s));
}

char d3net_error_code_char0(const d3net_unit_error_t *e) {
    return (char)d3net_uint_get(e->words, D3NET_CNT_UNIT_ERROR, 0, 8);
}

char d3net_error_code_char1(const d3net_unit_error_t *e) {
    return (char)d3net_uint_get(e->words, D3NET_CNT_UNIT_ERROR, 8, 8);
}

uint8_t d3net_error_subcode(const d3net_unit_error_t *e) {
    return (uint8_t)d3net_uint_get(e->words, D3NET_CNT_UNIT_ERROR, 16, 6);
}

bool d3net_error_error(const d3net_unit_error_t *e) {
    return d3net_bit_get(e->words, D3NET_CNT_UNIT_ERROR, 24);
}

bool d3net_error_alarm(const d3net_unit_error_t *e) {
    return d3net_bit_get(e->words, D3NET_CNT_UNIT_ERROR, 25);
}

bool d3net_error_warning(const d3net_unit_error_t *e) {
    return d3net_bit_get(e->words, D3NET_CNT_UNIT_ERROR, 26);
}

uint8_t d3net_error_unit_number(const d3net_unit_error_t *e) {
    return (uint8_t)d3net_uint_get(e->words, D3NET_CNT_UNIT_ERROR, 28, 4);
}
