#include "fw_update.h"

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

static void fw_set_message(const char* msg)
{
    if (msg != NULL)
    {
        status_set_str(STT_FW_UPDATE_MESSAGE, msg);
    }
}

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
    status_set_int(STT_FW_UPDATE_AVAILABLE, found ? 1 : 0);

    if (found)
    {
        ESP_LOGI(TAG, "Found %s (%ld bytes)", FW_UPDATE_FILEPATH, (long)st.st_size);
    }
    else
    {
        ESP_LOGI(TAG, "%s not found", FW_UPDATE_FILEPATH);
    }
}

esp_err_t fw_update_start(void)
{
    fw_set_message("Verifying update.bin");

    struct stat st = {0};
    if (stat(FW_UPDATE_FILEPATH, &st) != 0 || !S_ISREG(st.st_mode))
    {
        fw_set_message("update.bin not found");
        return ESP_ERR_NOT_FOUND;
    }

    if ((uint64_t)st.st_size < FW_UPDATE_HEADER_SIZE)
    {
        fw_set_message("Invalid update.bin size");
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target_ota = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || target_ota == NULL)
    {
        fw_set_message("OTA partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    const char* target_www_label = (target_ota->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? FW_UPDATE_WWW_1 : FW_UPDATE_WWW_0;
    const esp_partition_t* target_www = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, target_www_label);
    if (target_www == NULL)
    {
        fw_set_message("WWW partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    FILE* file = sdcard_file_open(FW_UPDATE_FILENAME, "rb");
    if (file == NULL)
    {
        fw_set_message("Failed to open update.bin");
        return ESP_FAIL;
    }

    fw_update_header_t header = {0};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header))
    {
        sdcard_file_close(file);
        fw_set_message("Failed to read header");
        return ESP_FAIL;
    }

    if (strncmp(header.magic, FW_UPDATE_MAGIC, sizeof(header.magic)) != 0 || header.version != FW_UPDATE_VERSION)
    {
        sdcard_file_close(file);
        fw_set_message("Invalid update.bin header");
        return ESP_ERR_INVALID_ARG;
    }

    if (header.app_size == 0 || header.app_size > target_ota->size || header.www_size == 0 || header.www_size > target_www->size)
    {
        sdcard_file_close(file);
        fw_set_message("Invalid image size");
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t expected_size = (uint64_t)FW_UPDATE_HEADER_SIZE + header.app_size + header.www_size;
    if ((uint64_t)st.st_size != expected_size)
    {
        sdcard_file_close(file);
        fw_set_message("update.bin size mismatch");
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t app_offset = FW_UPDATE_HEADER_SIZE;
    uint32_t www_offset = FW_UPDATE_HEADER_SIZE + header.app_size;

    if (!fw_verify_sha256(file, app_offset, header.app_size, header.app_sha256))
    {
        sdcard_file_close(file);
        fw_set_message("App checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    if (!fw_verify_sha256(file, www_offset, header.www_size, header.www_sha256))
    {
        sdcard_file_close(file);
        fw_set_message("WWW checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "Updating from %s/%s to %s/%s", running->label, fw_update_get_active_www(), target_ota->label, target_www->label);

    fw_set_message("Updating app");
    esp_err_t err = fw_write_app(file, target_ota, app_offset, header.app_size);
    if (err != ESP_OK)
    {
        sdcard_file_close(file);
        fw_set_message("App update failed");
        return err;
    }

    fw_set_message("Updating www");
    err = fw_write_www(file, target_www, www_offset, header.www_size);
    sdcard_file_close(file);
    if (err != ESP_OK)
    {
        fw_set_message("WWW update failed");
        return err;
    }

    err = esp_ota_set_boot_partition(target_ota);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        fw_set_message("Failed to set boot partition");
        return err;
    }

    fw_set_message("Update complete, rebooting");
    ESP_LOGI(TAG, "Firmware update complete, rebooting to %s/%s", target_ota->label, target_www->label);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}
