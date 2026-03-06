#include "ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define OTA_READ_CHUNK 1024

static const char *TAG = "ota_mgr";

struct ota_manager {
    app_config_t *cfg;
    SemaphoreHandle_t lock;
    ota_status_t st;
};

typedef struct {
    ota_manager_t *mgr;
    ota_target_t target;
    char url[APP_CFG_OTA_URL_LEN + 1];
} ota_job_ctx_t;

static void set_status_locked(ota_manager_t *mgr, bool active, bool success, const char *msg, int err)
{
    mgr->st.active = active;
    mgr->st.success = success;
    mgr->st.last_err = err;
    if (msg != NULL) {
        strlcpy(mgr->st.message, msg, sizeof(mgr->st.message));
    }
}

static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}

static esp_err_t begin_download_client(const char *url, esp_http_client_handle_t *out_client, int *out_total)
{
    esp_http_client_config_t hcfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = OTA_READ_CHUNK,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_fetch_headers(client);
    if (status_code < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int http_status = esp_http_client_get_status_code(client);
    if (http_status < 200 || http_status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_CONNECT;
    }

    *out_total = esp_http_client_get_content_length(client);
    *out_client = client;
    return ESP_OK;
}

static esp_err_t apply_ota_firmware(ota_manager_t *mgr, esp_http_client_handle_t client)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[OTA_READ_CHUNK];
    while (1) {
        int r = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (r < 0) {
            esp_ota_abort(handle);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }

        err = esp_ota_write(handle, buf, (size_t)r);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            return err;
        }

        if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            mgr->st.bytes_done += (uint32_t)r;
            xSemaphoreGive(mgr->lock);
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t apply_ota_spiffs(ota_manager_t *mgr, esp_http_client_handle_t client, int content_length)
{
    const esp_partition_t *spiffs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (spiffs == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (content_length > 0 && content_length > (int)spiffs->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Erase entire SPIFFS partition to make update robust for smaller images.
    esp_err_t err = esp_partition_erase_range(spiffs, 0, spiffs->size);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[OTA_READ_CHUNK];
    size_t offset = 0;
    while (1) {
        int r = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (r < 0) {
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }

        if (offset + (size_t)r > spiffs->size) {
            return ESP_ERR_INVALID_SIZE;
        }

        err = esp_partition_write(spiffs, offset, buf, (size_t)r);
        if (err != ESP_OK) {
            return err;
        }

        offset += (size_t)r;
        if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            mgr->st.bytes_done = (uint32_t)offset;
            xSemaphoreGive(mgr->lock);
        }
    }

    if (content_length > 0 && offset != (size_t)content_length) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void ota_job_task(void *arg)
{
    ota_job_ctx_t *job = (ota_job_ctx_t *)arg;
    ota_manager_t *mgr = job->mgr;

    int content_length = 0;
    esp_http_client_handle_t client = NULL;
    esp_err_t err = begin_download_client(job->url, &client, &content_length);
    if (err != ESP_OK) {
        if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            set_status_locked(mgr, false, false, "download open failed", err);
            xSemaphoreGive(mgr->lock);
        }
        free(job);
        vTaskDelete(NULL);
        return;
    }

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        mgr->st.bytes_total = (content_length > 0) ? (uint32_t)content_length : 0;
        xSemaphoreGive(mgr->lock);
    }

    if (job->target == OTA_TARGET_FIRMWARE) {
        err = apply_ota_firmware(mgr, client);
    } else {
        err = apply_ota_spiffs(mgr, client, content_length);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (err == ESP_OK) {
            mgr->st.reboot_pending = true;
            set_status_locked(mgr, false, true, "OTA successful, rebooting", ESP_OK);
        } else {
            set_status_locked(mgr, false, false, "OTA failed", err);
        }
        xSemaphoreGive(mgr->lock);
    }

    if (err == ESP_OK) {
        xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 4, NULL);
    }

    free(job);
    vTaskDelete(NULL);
}

esp_err_t ota_manager_init(ota_manager_t **out_mgr, const ota_manager_config_t *cfg)
{
    if (out_mgr == NULL || cfg == NULL || cfg->cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (mgr == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mgr->cfg = cfg->cfg;
    mgr->lock = xSemaphoreCreateMutex();
    if (mgr->lock == NULL) {
        free(mgr);
        return ESP_ERR_NO_MEM;
    }

    mgr->st.active = false;
    mgr->st.success = false;
    mgr->st.reboot_pending = false;
    mgr->st.target = OTA_TARGET_FIRMWARE;
    mgr->st.bytes_done = 0;
    mgr->st.bytes_total = 0;
    mgr->st.last_err = ESP_OK;
    strlcpy(mgr->st.message, "idle", sizeof(mgr->st.message));

    *out_mgr = mgr;
    return ESP_OK;
}

void ota_manager_deinit(ota_manager_t *mgr)
{
    if (mgr == NULL) {
        return;
    }

    if (mgr->lock != NULL) {
        vSemaphoreDelete(mgr->lock);
    }
    free(mgr);
}

esp_err_t ota_manager_start(ota_manager_t *mgr, ota_target_t target, const char *url)
{
    if (mgr == NULL || (target != OTA_TARGET_FIRMWARE && target != OTA_TARGET_FILESYSTEM)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *resolved = url;
    if (resolved == NULL || resolved[0] == '\0') {
        resolved = (target == OTA_TARGET_FIRMWARE) ? mgr->cfg->ota_firmware_url : mgr->cfg->ota_filesystem_url;
    }

    if (resolved == NULL || resolved[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ota_job_ctx_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }
    job->mgr = mgr;
    job->target = target;
    strlcpy(job->url, resolved, sizeof(job->url));

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(job);
        return ESP_ERR_TIMEOUT;
    }

    if (mgr->st.active) {
        xSemaphoreGive(mgr->lock);
        free(job);
        return ESP_ERR_INVALID_STATE;
    }

    mgr->st.active = true;
    mgr->st.success = false;
    mgr->st.reboot_pending = false;
    mgr->st.target = target;
    mgr->st.bytes_done = 0;
    mgr->st.bytes_total = 0;
    mgr->st.last_err = ESP_OK;
    strlcpy(mgr->st.url, resolved, sizeof(mgr->st.url));
    strlcpy(mgr->st.message, "starting", sizeof(mgr->st.message));
    xSemaphoreGive(mgr->lock);

    BaseType_t ok = xTaskCreate(ota_job_task, "ota_job", 8192, job, 5, NULL);
    if (ok != pdPASS) {
        if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            set_status_locked(mgr, false, false, "task create failed", ESP_ERR_NO_MEM);
            xSemaphoreGive(mgr->lock);
        }
        free(job);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "OTA started target=%s url=%s", target == OTA_TARGET_FIRMWARE ? "firmware" : "filesystem", resolved);
    return ESP_OK;
}

void ota_manager_get_status(ota_manager_t *mgr, ota_status_t *out_status)
{
    if (mgr == NULL || out_status == NULL) {
        return;
    }

    if (xSemaphoreTake(mgr->lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        *out_status = mgr->st;
        xSemaphoreGive(mgr->lock);
    }
}
