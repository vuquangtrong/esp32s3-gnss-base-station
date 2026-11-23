#include "fw_update.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sdcard.h"
#include "status.h"

#define FW_UPDATE_FILEPATH SDCARD_MOUNT_POINT "/" FW_UPDATE_FILENAME
#define FW_UPDATE_WWW_0    "www_0"
#define FW_UPDATE_WWW_1    "www_1"

static const char* TAG = "fw_update";

static uint8_t s_buf[FW_UPDATE_BUFFER_SIZE];

static bool fw_verify_sha256(FILE* file, uint32_t offset, uint32_t size, const uint8_t expected[FW_UPDATE_SHA256_LEN])
{
    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return false;
    }

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS)
    {
        return false;
    }

    uint32_t remaining = size;
    while (remaining > 0)
    {
        size_t to_read = (remaining > sizeof(s_buf)) ? sizeof(s_buf) : (size_t)remaining;
        size_t n = fread(s_buf, 1, to_read, file);
        if (n != to_read)
        {
            psa_hash_abort(&op);
            return false;
        }
        if (psa_hash_update(&op, s_buf, n) != PSA_SUCCESS)
        {
            psa_hash_abort(&op);
            return false;
        }
        remaining -= (uint32_t)n;
    }

    uint8_t digest[FW_UPDATE_SHA256_LEN];
    size_t digest_len = 0;
    psa_status_t status = psa_hash_finish(&op, digest, sizeof(digest), &digest_len);
    if (status != PSA_SUCCESS || digest_len != sizeof(digest))
    {
        psa_hash_abort(&op);
        return false;
    }

    return memcmp(digest, expected, sizeof(digest)) == 0;
}

static esp_err_t fw_write_app(FILE* file, const esp_partition_t* target_ota, uint32_t offset, uint32_t size)
{
    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target_ota, size, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t remaining = size;
    while (remaining > 0)
    {
        size_t to_read = (remaining > sizeof(s_buf)) ? sizeof(s_buf) : (size_t)remaining;
        size_t n = fread(s_buf, 1, to_read, file);
        if (n != to_read)
        {
            esp_ota_abort(ota_handle);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, s_buf, n);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            return err;
        }
        remaining -= (uint32_t)n;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t fw_write_www(FILE* file, const esp_partition_t* target_www, uint32_t offset, uint32_t size)
{
    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    esp_err_t err = esp_partition_erase_range(target_www, 0, target_www->size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_partition_erase_range failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t write_offset = 0;
    uint32_t remaining = size;
    while (remaining > 0)
    {
        size_t to_read = (remaining > sizeof(s_buf)) ? sizeof(s_buf) : (size_t)remaining;
        size_t n = fread(s_buf, 1, to_read, file);
        if (n != to_read)
        {
            return ESP_FAIL;
        }

        err = esp_partition_write(target_www, write_offset, s_buf, n);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_partition_write failed: %s", esp_err_to_name(err));
            return err;
        }

        write_offset += (uint32_t)n;
        remaining -= (uint32_t)n;
    }

    return ESP_OK;
}

const char* fw_update_get_active_app(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    return (running != NULL) ? running->label : "unknown";
}

const char* fw_update_get_active_www(void)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == NULL)
    {
        return "unknown";
    }

    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
    {
        return FW_UPDATE_WWW_1;
    }
    return FW_UPDATE_WWW_0;
}

void fw_update_check(void)
{
    struct stat st = {0};
    bool found = (stat(FW_UPDATE_FILEPATH, &st) == 0) && S_ISREG(st.st_mode);
    if (found)
    {
        status_set_int(STT_FW_UPDATE_AVAILABLE, 1);
        ESP_LOGI(TAG, "Found %s (%ld bytes)", FW_UPDATE_FILEPATH, (long)st.st_size);
    }
    else
    {
        status_set_int(STT_FW_UPDATE_AVAILABLE, 0);
        ESP_LOGI(TAG, "%s not found", FW_UPDATE_FILEPATH);
    }
    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_IDLE);
}

static esp_err_t fw_update_fail(FILE* file, esp_err_t err, const char* format, ...)
{
    char msg[128];
    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    ESP_LOGE(TAG, "%s", msg);

    if (file != NULL)
    {
        sdcard_file_close(file);
    }
    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_FAILED);
    return err;
}

static esp_err_t fw_update_run(void)
{
    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_VERIFYING);

    FILE* file = NULL;

    struct stat st = {0};
    if (stat(FW_UPDATE_FILEPATH, &st) != 0 || !S_ISREG(st.st_mode))
    {
        return fw_update_fail(NULL, ESP_ERR_NOT_FOUND, "%s not found", FW_UPDATE_FILEPATH);
    }

    if ((uint64_t)st.st_size < FW_UPDATE_HEADER_SIZE)
    {
        return fw_update_fail(NULL, ESP_ERR_INVALID_SIZE, "Invalid %s size", FW_UPDATE_FILENAME);
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target_ota = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || target_ota == NULL)
    {
        return fw_update_fail(NULL, ESP_ERR_NOT_FOUND, "OTA partition not found");
    }

    const char* target_www_label = (target_ota->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? FW_UPDATE_WWW_1 : FW_UPDATE_WWW_0;
    const esp_partition_t* target_www = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, target_www_label);
    if (target_www == NULL)
    {
        return fw_update_fail(NULL, ESP_ERR_NOT_FOUND, "WWW partition %s not found", target_www_label);
    }

    file = sdcard_file_open(FW_UPDATE_FILENAME, "rb");
    if (file == NULL)
    {
        return fw_update_fail(NULL, ESP_FAIL, "Failed to open %s", FW_UPDATE_FILENAME);
    }

    fw_update_header_t header = {0};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header))
    {
        return fw_update_fail(file, ESP_FAIL, "Failed to read header");
    }

    if (strncmp(header.magic, FW_UPDATE_MAGIC, sizeof(header.magic)) != 0 || header.version != FW_UPDATE_VERSION)
    {
        return fw_update_fail(file, ESP_ERR_INVALID_ARG, "Invalid %s header", FW_UPDATE_FILENAME);
    }

    if (header.app_size == 0 || header.app_size > target_ota->size || header.www_size == 0 || header.www_size > target_www->size)
    {
        return fw_update_fail(file, ESP_ERR_INVALID_SIZE, "Invalid image size");
    }

    uint64_t expected_size = (uint64_t)FW_UPDATE_HEADER_SIZE + header.app_size + header.www_size;
    if ((uint64_t)st.st_size != expected_size)
    {
        return fw_update_fail(file, ESP_ERR_INVALID_SIZE, "%s size mismatch", FW_UPDATE_FILENAME);
    }

    uint32_t app_offset = FW_UPDATE_HEADER_SIZE;
    uint32_t www_offset = FW_UPDATE_HEADER_SIZE + header.app_size;

    if (!fw_verify_sha256(file, app_offset, header.app_size, header.app_sha256))
    {
        return fw_update_fail(file, ESP_ERR_INVALID_CRC, "App checksum mismatch");
    }

    if (!fw_verify_sha256(file, www_offset, header.www_size, header.www_sha256))
    {
        return fw_update_fail(file, ESP_ERR_INVALID_CRC, "WWW checksum mismatch");
    }

    ESP_LOGI(TAG, "Updating from %s/%s to %s/%s", running->label, fw_update_get_active_www(), target_ota->label, target_www->label);

    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_UPDATING_APP);
    esp_err_t err = fw_write_app(file, target_ota, app_offset, header.app_size);
    if (err != ESP_OK)
    {
        return fw_update_fail(file, err, "App update failed");
    }

    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_UPDATING_WWW);
    err = fw_write_www(file, target_www, www_offset, header.www_size);
    if (err != ESP_OK)
    {
        return fw_update_fail(file, err, "WWW update failed");
    }

    err = esp_ota_set_boot_partition(target_ota);
    if (err != ESP_OK)
    {
        return fw_update_fail(file, err, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    }

    status_set_int(STT_FW_UPDATE_STATUS, FW_UPDATE_SUCCESS);
    ESP_LOGI(TAG, "Firmware update complete, rebooting to %s/%s", target_ota->label, target_www->label);
    sdcard_file_close(file);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

static void fw_update_task(void* arg)
{
    fw_update_run();
    vTaskDelete(NULL);
}

esp_err_t fw_update_start(void)
{
    if (status_get_int(STT_FW_UPDATE_AVAILABLE) == 0 || status_get_int(STT_FW_UPDATE_STATUS) != FW_UPDATE_IDLE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(fw_update_task, "fw_update", 8192, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create firmware update task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
