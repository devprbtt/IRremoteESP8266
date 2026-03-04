#include "d3net_gateway.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define D3NET_UNIT_ID_STR_LEN 6

static const char *TAG = "d3net_gateway";

static void d3net_throttle(d3net_gateway_t *gw, uint64_t now_ms) {
    if (gw->last_op_ms == 0U) {
        return;
    }
    const uint64_t elapsed = now_ms - gw->last_op_ms;
    if (elapsed >= gw->throttle_ms) {
        return;
    }
    const uint32_t wait_ms = (uint32_t)(gw->throttle_ms - elapsed);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
}

static esp_err_t d3net_read(
    d3net_gateway_t *gw,
    d3net_reg_type_t reg_type,
    uint16_t address,
    uint16_t count,
    uint16_t *out_words,
    uint64_t now_ms) {
    d3net_throttle(gw, now_ms);
    esp_err_t err = gw->read_fn(gw->io_ctx, reg_type, address, count, out_words);
    gw->last_op_ms = now_ms;
    return err;
}

static esp_err_t d3net_write(
    d3net_gateway_t *gw,
    uint16_t address,
    uint16_t count,
    const uint16_t *words,
    uint64_t now_ms) {
    d3net_throttle(gw, now_ms);
    esp_err_t err = gw->write_fn(gw->io_ctx, address, count, words);
    gw->last_op_ms = now_ms;
    return err;
}

static void d3net_make_unit_id(uint8_t index, char *out) {
    const uint8_t group = (uint8_t)(index / 16U + 1U);
    const uint8_t num = (uint8_t)(index % 16U);
    snprintf(out, D3NET_UNIT_ID_STR_LEN, "%u-%02u", group, num);
}

void d3net_gateway_init(
    d3net_gateway_t *gw,
    d3net_read_registers_fn read_fn,
    d3net_write_registers_fn write_fn,
    void *io_ctx,
    uint8_t modbus_device_id) {
    memset(gw, 0, sizeof(*gw));
    gw->read_fn = read_fn;
    gw->write_fn = write_fn;
    gw->io_ctx = io_ctx;
    gw->modbus_device_id = modbus_device_id;
    gw->poll_interval_s = D3NET_DEFAULT_POLL_INTERVAL_S;
    gw->throttle_ms = D3NET_DEFAULT_THROTTLE_MS;
    gw->cache_write_s = D3NET_DEFAULT_CACHE_WRITE_S;
    gw->cache_error_s = D3NET_DEFAULT_CACHE_ERROR_S;
}

esp_err_t d3net_gateway_discover_units(d3net_gateway_t *gw) {
    if (gw == NULL || gw->read_fn == NULL || gw->write_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    esp_err_t err = d3net_read(
        gw,
        D3NET_REG_INPUT,
        D3NET_ADDR_SYSTEM_STATUS,
        D3NET_CNT_SYSTEM_STATUS,
        gw->system_status.words,
        now_ms);
    if (err != ESP_OK) {
        return err;
    }

    gw->discovered_count = 0;
    for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
        d3net_unit_t *unit = &gw->units[i];
        memset(unit, 0, sizeof(*unit));
        unit->index = i;
        d3net_make_unit_id(i, unit->unit_id);

        if (!d3net_system_unit_connected(&gw->system_status, i) ||
            d3net_system_unit_error(&gw->system_status, i)) {
            continue;
        }

        const uint16_t cap_addr = (uint16_t)(D3NET_ADDR_UNIT_CAP + i * D3NET_CNT_UNIT_CAP);
        err = d3net_read(gw, D3NET_REG_INPUT, cap_addr, D3NET_CNT_UNIT_CAP, unit->cap.words, now_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cap read failed for unit %u: %s", i, esp_err_to_name(err));
            continue;
        }

        const uint16_t status_addr = (uint16_t)(D3NET_ADDR_UNIT_STATUS + i * D3NET_CNT_UNIT_STATUS);
        err = d3net_read(gw, D3NET_REG_INPUT, status_addr, D3NET_CNT_UNIT_STATUS, unit->status.words, now_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "status read failed for unit %u: %s", i, esp_err_to_name(err));
            continue;
        }

        unit->present = true;
        gw->discovered_count++;
    }

    ESP_LOGI(TAG, "discovered %u units", gw->discovered_count);
    return ESP_OK;
}

esp_err_t d3net_gateway_poll_status(d3net_gateway_t *gw) {
    if (gw == NULL || gw->read_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    for (uint8_t i = 0; i < D3NET_MAX_UNITS; i++) {
        d3net_unit_t *unit = &gw->units[i];
        if (!unit->present) {
            continue;
        }
        if (d3net_holding_write_within(&unit->holding, now_ms, gw->cache_write_s)) {
            continue;
        }

        const uint16_t status_addr = (uint16_t)(D3NET_ADDR_UNIT_STATUS + i * D3NET_CNT_UNIT_STATUS);
        esp_err_t err = d3net_read(
            gw,
            D3NET_REG_INPUT,
            status_addr,
            D3NET_CNT_UNIT_STATUS,
            unit->status.words,
            now_ms);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "status poll failed for unit %u: %s", i, esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

esp_err_t d3net_gateway_read_error(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    if (gw == NULL || unit == NULL || !unit->present) {
        return ESP_ERR_INVALID_ARG;
    }
    if (unit->last_error_read_ms != 0U &&
        (now_ms - unit->last_error_read_ms) < ((uint64_t)gw->cache_error_s * 1000ULL)) {
        return ESP_OK;
    }

    const uint16_t error_addr = (uint16_t)(D3NET_ADDR_UNIT_ERROR + unit->index * D3NET_CNT_UNIT_ERROR);
    esp_err_t err = d3net_read(
        gw,
        D3NET_REG_INPUT,
        error_addr,
        D3NET_CNT_UNIT_ERROR,
        unit->error.words,
        now_ms);
    if (err == ESP_OK) {
        unit->last_error_read_ms = now_ms;
    }
    return err;
}

static esp_err_t d3net_holding_read(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    const uint16_t holding_addr = (uint16_t)(D3NET_ADDR_UNIT_HOLDING + unit->index * D3NET_CNT_UNIT_HOLDING);
    esp_err_t err = d3net_read(
        gw,
        D3NET_REG_HOLDING,
        holding_addr,
        D3NET_CNT_UNIT_HOLDING,
        unit->holding.words,
        now_ms);
    if (err == ESP_OK) {
        d3net_holding_mark_read(&unit->holding, now_ms);
    }
    return err;
}

static esp_err_t d3net_holding_write_if_dirty(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    if (!unit->holding.dirty) {
        return ESP_OK;
    }
    const uint16_t holding_addr = (uint16_t)(D3NET_ADDR_UNIT_HOLDING + unit->index * D3NET_CNT_UNIT_HOLDING);
    esp_err_t err = d3net_write(
        gw,
        holding_addr,
        D3NET_CNT_UNIT_HOLDING,
        unit->holding.words,
        now_ms);
    if (err == ESP_OK) {
        d3net_holding_mark_written(&unit->holding, now_ms);
    }
    return err;
}

esp_err_t d3net_unit_prepare_write(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    if (gw == NULL || unit == NULL || !unit->present) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool must_reload_holding =
        (unit->holding.last_read_ms == 0U) ||
        (!unit->holding.dirty &&
         !d3net_holding_read_within(&unit->holding, now_ms, gw->cache_write_s) &&
         !d3net_holding_write_within(&unit->holding, now_ms, gw->cache_write_s));

    if (!must_reload_holding) {
        return ESP_OK;
    }

    esp_err_t err = d3net_holding_read(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }

    d3net_holding_sync_from_status(&unit->holding, &unit->status);
    if (unit->holding.dirty) {
        return d3net_holding_write_if_dirty(gw, unit, now_ms);
    }
    return ESP_OK;
}

esp_err_t d3net_unit_commit_write(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    if (gw == NULL || unit == NULL || !unit->present) {
        return ESP_ERR_INVALID_ARG;
    }

    d3net_holding_sync_from_status(&unit->holding, &unit->status);
    esp_err_t err = d3net_holding_write_if_dirty(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }

    if (d3net_holding_filter_reset_get(&unit->holding)) {
        d3net_holding_filter_reset_set(&unit->holding, false);
        err = d3net_holding_write_if_dirty(gw, unit, now_ms);
    }
    return err;
}

esp_err_t d3net_unit_set_power(d3net_gateway_t *gw, d3net_unit_t *unit, bool on, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_status_power_set(&unit->status, on);
    return d3net_unit_commit_write(gw, unit, now_ms);
}

esp_err_t d3net_unit_set_mode(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_mode_t mode, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_status_power_set(&unit->status, true);
    d3net_status_oper_mode_set(&unit->status, mode);
    return d3net_unit_commit_write(gw, unit, now_ms);
}

esp_err_t d3net_unit_set_setpoint(d3net_gateway_t *gw, d3net_unit_t *unit, float celsius, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_status_temp_setpoint_set(&unit->status, celsius);
    return d3net_unit_commit_write(gw, unit, now_ms);
}

esp_err_t d3net_unit_set_fan_speed(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_fan_speed_t speed, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_status_fan_speed_set(&unit->status, speed);
    return d3net_unit_commit_write(gw, unit, now_ms);
}

esp_err_t d3net_unit_set_fan_dir(d3net_gateway_t *gw, d3net_unit_t *unit, d3net_fan_dir_t dir, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_status_fan_dir_set(&unit->status, dir);
    return d3net_unit_commit_write(gw, unit, now_ms);
}

esp_err_t d3net_unit_filter_reset(d3net_gateway_t *gw, d3net_unit_t *unit, uint64_t now_ms) {
    esp_err_t err = d3net_unit_prepare_write(gw, unit, now_ms);
    if (err != ESP_OK) {
        return err;
    }
    d3net_holding_filter_reset_set(&unit->holding, true);
    return d3net_unit_commit_write(gw, unit, now_ms);
}
