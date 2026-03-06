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

static inline bool bit_get(const uint8_t *bytes, int bit)
{
    return (bytes[bit / 8] & (1U << (bit % 8))) != 0;
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
    uint16_t input[2] = {0};

    esp_err_t err;

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
    z->last_update_ms = (uint64_t)(esp_timer_get_time() / 1000);
    z->last_error[0] = '\0';

    return ESP_OK;
}

static esp_err_t write_verify_power(hvac_manager_t *mgr, hvac_zone_state_t *z, bool value)
{
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
    } else if (err == ESP_ERR_INVALID_STATE) {
        snprintf(msg, msg_len, "mode rate-limited");
    } else {
        snprintf(msg, msg_len, "write failed: 0x%x", err);
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
