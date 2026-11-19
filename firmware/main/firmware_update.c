#include "firmware_update.h"

#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "server.h"
#include "status.h"

static const char* TAG = "fw_update";

static volatile firmware_update_state_t g_state = FW_UPDATE_IDLE;

static void firmware_update_stage_task(void* arg);
static void firmware_update_install_task(void* arg);

static void firmware_update_set_state(firmware_update_state_t state)
{
    g_state = state;
    status_set(STT_FW_UPDATE_STATE, (int)state);
}

static void firmware_update_set_progress(uint32_t progress)
{
    uint32_t value = (progress > 100) ? 100 : progress;
    status_set(STT_FW_UPDATE_PROGRESS, (int)value);
}

static void firmware_update_set_message(const char* message)
{
    status_set(STT_FW_UPDATE_MESSAGE, message);
}

static void firmware_update_fail(const char* message)
{
    ESP_LOGE(TAG, "%s", message);
    firmware_update_set_message(message);
    firmware_update_set_state(FW_UPDATE_ERROR);
}

static uint32_t firmware_update_crc32_update(uint32_t crc, const uint8_t* data, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

static uint32_t firmware_update_file_crc(FILE* file, uint32_t offset, uint32_t size)
{
    static uint8_t s_buffer[FIRMWARE_UPDATE_BUFFER_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t remaining = size;

    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return 0;
    }

    while (remaining > 0)
    {
        size_t chunk = (remaining > sizeof(s_buffer)) ? sizeof(s_buffer) : (size_t)remaining;
        size_t read_bytes = fread(s_buffer, 1, chunk, file);
        if (read_bytes == 0)
        {
            return 0;
        }
        crc = firmware_update_crc32_update(crc, s_buffer, read_bytes);
        remaining -= (uint32_t)read_bytes;
    }

    return crc ^ 0xFFFFFFFFU;
}

static uint32_t firmware_update_partition_crc(const esp_partition_t* partition, uint32_t size)
{
    static uint8_t s_buffer[FIRMWARE_UPDATE_BUFFER_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t offset = 0;
    uint32_t remaining = size;

    while (remaining > 0)
    {
        size_t want = (remaining > sizeof(s_buffer)) ? sizeof(s_buffer) : (size_t)remaining;
        size_t read_len = (want + 3) & ~((size_t)3);
        if (esp_partition_read(partition, offset, s_buffer, read_len) != ESP_OK)
        {
            return 0;
        }
        crc = firmware_update_crc32_update(crc, s_buffer, want);
        offset += (uint32_t)want;
        remaining -= (uint32_t)want;
    }

    return crc ^ 0xFFFFFFFFU;
}

static FILE* firmware_update_open_package(void)
{
    char filepath[128] = {0};
    snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, FIRMWARE_UPDATE_FILENAME);
    return fopen(filepath, "rb");
}

static const esp_partition_t* firmware_update_find_app_partition(const char* label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
}

static const esp_partition_t* firmware_update_find_www_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, WWW_PARTITION_LABEL);
}

static esp_err_t firmware_update_read_header(FILE* file, firmware_package_header_t* header)
{
    if (file == NULL || header == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(header, 0, sizeof(*header));
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }
    if (fread(header, 1, sizeof(*header), file) != sizeof(*header))
    {
        return ESP_FAIL;
    }

    if (memcmp(header->magic, FIRMWARE_PACKAGE_MAGIC, sizeof(header->magic)) != 0)
    {
        ESP_LOGE(TAG, "Invalid package magic");
        return ESP_ERR_INVALID_ARG;
    }
    if (header->format_version != FIRMWARE_PACKAGE_VERSION)
    {
        ESP_LOGE(TAG, "Unsupported package version: %u", header->format_version);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t* factory = firmware_update_find_app_partition("factory");
    const esp_partition_t* ota = firmware_update_find_app_partition("ota_0");
    const esp_partition_t* www = firmware_update_find_www_partition();
    if (factory == NULL || ota == NULL || www == NULL)
    {
        ESP_LOGE(TAG, "Required partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    if (header->app_size == 0 || header->app_size > factory->size || header->app_size > ota->size)
    {
        ESP_LOGE(TAG, "Invalid app size: %u", header->app_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->www_size == 0 || header->www_size > www->size)
    {
        ESP_LOGE(TAG, "Invalid www size: %u", header->www_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t app_end = (uint64_t)header->app_offset + header->app_size;
    uint64_t www_end = (uint64_t)header->www_offset + header->www_size;
    if (header->app_offset < FIRMWARE_PACKAGE_HEADER_SIZE || header->www_offset < app_end ||
        app_end > header->total_size || www_end > header->total_size)
    {
        ESP_LOGE(TAG, "Invalid package offsets or total size");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t image_magic = 0;
    if (fseek(file, (long)header->app_offset, SEEK_SET) != 0 || fread(&image_magic, 1, 1, file) != 1)
    {
        return ESP_FAIL;
    }
    if (image_magic != ESP_IMAGE_HEADER_MAGIC)
    {
        ESP_LOGE(TAG, "App image magic mismatch");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

void firmware_update_refresh_availability(void)
{
    bool available = false;

    if (status_get_int(STT_SDCARD_STATUS) == SDCARD_MOUNTED)
    {
        char filepath[128] = {0};
        snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, FIRMWARE_UPDATE_FILENAME);
        struct stat st = {0};
        if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > (off_t)FIRMWARE_PACKAGE_HEADER_SIZE)
        {
            available = true;
        }
    }

    status_set(STT_FW_UPDATE_AVAILABLE, available ? 1 : 0);
}

esp_err_t firmware_update_check(uint32_t* file_size)
{
    if (file_size != NULL)
    {
        *file_size = 0;
    }
    if (status_get_int(STT_SDCARD_STATUS) != SDCARD_MOUNTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    FILE* file = firmware_update_open_package();
    if (file == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    firmware_package_header_t header = {0};
    esp_err_t err = firmware_update_read_header(file, &header);
    if (err == ESP_OK && file_size != NULL)
    {
        *file_size = header.total_size;
    }
    fclose(file);
    return err;
}

esp_err_t firmware_update_start(void)
{
    if (g_state != FW_UPDATE_IDLE && g_state != FW_UPDATE_ERROR)
    {
        ESP_LOGW(TAG, "Update already in progress, state: %d", (int)g_state);
        return ESP_ERR_INVALID_STATE;
    }
    if (status_get_int(STT_SDCARD_STATUS) != SDCARD_MOUNTED)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (status_get_int(STT_LOGGER_STATUS) == LOGGER_RUNNING)
    {
        ESP_LOGW(TAG, "Cannot update while logger is running");
        return ESP_ERR_INVALID_STATE;
    }

    firmware_update_set_state(FW_UPDATE_CHECKING);
    firmware_update_set_progress(0);
    firmware_update_set_message("Checking update file");

    FILE* file = firmware_update_open_package();
    if (file == NULL)
    {
        firmware_update_fail("Update file not found");
        return ESP_ERR_NOT_FOUND;
    }

    firmware_package_header_t header = {0};
    esp_err_t err = firmware_update_read_header(file, &header);
    fclose(file);
    if (err != ESP_OK)
    {
        firmware_update_fail("Invalid update file");
        return err;
    }

    firmware_update_set_state(FW_UPDATE_STAGING);
    firmware_update_set_message("Staging update");

    if (xTaskCreate(firmware_update_stage_task, "fw_stage", 8192, NULL, 2, NULL) != pdPASS)
    {
        firmware_update_fail("Failed to start update task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void firmware_update_stage_task(void* arg)
{
    (void)arg;

    FILE* file = firmware_update_open_package();
    if (file == NULL)
    {
        firmware_update_fail("Update file not found");
        vTaskDelete(NULL);
        return;
    }

    firmware_package_header_t header = {0};
    if (firmware_update_read_header(file, &header) != ESP_OK)
    {
        fclose(file);
        firmware_update_fail("Invalid update file");
        vTaskDelete(NULL);
        return;
    }

    firmware_update_set_state(FW_UPDATE_CHECKING);
    firmware_update_set_message("Verifying update file");
    firmware_update_set_progress(5);

    uint32_t app_crc = firmware_update_file_crc(file, header.app_offset, header.app_size);
    uint32_t www_crc = firmware_update_file_crc(file, header.www_offset, header.www_size);
    if (app_crc != header.app_crc32 || www_crc != header.www_crc32)
    {
        fclose(file);
        firmware_update_fail("Update file checksum mismatch");
        vTaskDelete(NULL);
        return;
    }

    const esp_partition_t* ota = firmware_update_find_app_partition("ota_0");
    if (ota == NULL)
    {
        fclose(file);
        firmware_update_fail("OTA partition not found");
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(ota, header.app_size, &handle);
    if (err != ESP_OK)
    {
        fclose(file);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        firmware_update_fail("Failed to start OTA write");
        vTaskDelete(NULL);
        return;
    }

    firmware_update_set_state(FW_UPDATE_STAGING);
    firmware_update_set_message("Writing update to OTA slot");

    static uint8_t s_buffer[FIRMWARE_UPDATE_BUFFER_SIZE];
    uint32_t remaining = header.app_size;
    uint32_t written = 0;

    if (fseek(file, (long)header.app_offset, SEEK_SET) != 0)
    {
        err = ESP_FAIL;
    }
    while (err == ESP_OK && remaining > 0)
    {
        size_t chunk = (remaining > sizeof(s_buffer)) ? sizeof(s_buffer) : (size_t)remaining;
        size_t read_bytes = fread(s_buffer, 1, chunk, file);
        if (read_bytes == 0)
        {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, s_buffer, read_bytes);
        if (err != ESP_OK)
        {
            break;
        }
        written += (uint32_t)read_bytes;
        remaining -= (uint32_t)read_bytes;
        firmware_update_set_progress(10 + (uint32_t)(80ULL * written / header.app_size));
        vTaskDelay(1);
    }
    fclose(file);

    esp_err_t end_err = esp_ota_end(handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
        firmware_update_fail("Failed to write OTA slot");
        vTaskDelete(NULL);
        return;
    }
    if (end_err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(end_err));
        firmware_update_fail("OTA image verification failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(ota);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        firmware_update_fail("Failed to set boot partition");
        vTaskDelete(NULL);
        return;
    }

    firmware_update_set_state(FW_UPDATE_REBOOTING);
    firmware_update_set_progress(100);
    firmware_update_set_message("Update staged, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t firmware_update_write_partition(const esp_partition_t* partition, FILE* file, uint32_t offset,
                                                 uint32_t size, uint32_t progress_base, uint32_t progress_range)
{
    uint32_t erase_size = (size + partition->erase_size - 1) & ~(partition->erase_size - 1);
    esp_err_t err = esp_partition_erase_range(partition, 0, erase_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase partition %s: %s", partition->label, esp_err_to_name(err));
        return err;
    }

    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    static uint8_t s_buffer[FIRMWARE_UPDATE_BUFFER_SIZE];
    uint32_t remaining = size;
    uint32_t written = 0;

    while (remaining > 0)
    {
        size_t want = (remaining > sizeof(s_buffer)) ? sizeof(s_buffer) : (size_t)remaining;
        size_t read_bytes = fread(s_buffer, 1, want, file);
        if (read_bytes == 0)
        {
            return ESP_FAIL;
        }

        size_t write_len = (read_bytes + 3) & ~((size_t)3);
        memset(s_buffer + read_bytes, 0xFF, write_len - read_bytes);

        err = esp_partition_write(partition, written, s_buffer, write_len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to write partition %s: %s", partition->label, esp_err_to_name(err));
            return err;
        }

        written += (uint32_t)write_len;
        remaining -= (uint32_t)read_bytes;

        uint32_t progress = progress_base + (uint32_t)((uint64_t)progress_range * written / size);
        firmware_update_set_progress(progress > 95 ? 95 : progress);
        vTaskDelay(1);
    }

    return ESP_OK;
}

static void firmware_update_restore_factory(const char* reason)
{
    ESP_LOGE(TAG, "Installation failed: %s", reason);
    firmware_update_set_message(reason);
    firmware_update_set_state(FW_UPDATE_ERROR);

    const esp_partition_t* factory = firmware_update_find_app_partition("factory");
    if (factory != NULL)
    {
        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to restore factory boot: %s", esp_err_to_name(err));
        }
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void firmware_update_install_task(void* arg)
{
    (void)arg;

    firmware_update_set_state(FW_UPDATE_INSTALLING);
    firmware_update_set_progress(0);
    firmware_update_set_message("Installing firmware");

    FILE* file = firmware_update_open_package();
    if (file == NULL)
    {
        firmware_update_restore_factory("Update file not found");
        return;
    }

    firmware_package_header_t header = {0};
    if (firmware_update_read_header(file, &header) != ESP_OK)
    {
        fclose(file);
        firmware_update_restore_factory("Invalid update file");
        return;
    }

    firmware_update_set_message("Verifying update file");
    uint32_t app_crc = firmware_update_file_crc(file, header.app_offset, header.app_size);
    uint32_t www_crc = firmware_update_file_crc(file, header.www_offset, header.www_size);
    if (app_crc != header.app_crc32 || www_crc != header.www_crc32)
    {
        fclose(file);
        firmware_update_restore_factory("Update file checksum mismatch");
        return;
    }

    const esp_partition_t* factory = firmware_update_find_app_partition("factory");
    const esp_partition_t* www = firmware_update_find_www_partition();
    if (factory == NULL || www == NULL)
    {
        fclose(file);
        firmware_update_restore_factory("Target partition not found");
        return;
    }

    firmware_update_set_message("Writing firmware");
    esp_err_t err = firmware_update_write_partition(factory, file, header.app_offset, header.app_size, 0, 50);
    if (err != ESP_OK)
    {
        fclose(file);
        firmware_update_restore_factory("Failed to write firmware");
        return;
    }

    firmware_update_set_message("Writing web interface");
    err = firmware_update_write_partition(www, file, header.www_offset, header.www_size, 50, 40);
    if (err != ESP_OK)
    {
        fclose(file);
        firmware_update_restore_factory("Failed to write web interface");
        return;
    }

    firmware_update_set_message("Verifying written partitions");
    firmware_update_set_progress(92);
    uint32_t factory_crc = firmware_update_partition_crc(factory, header.app_size);
    uint32_t written_www_crc = firmware_update_partition_crc(www, header.www_size);
    if (factory_crc != header.app_crc32 || written_www_crc != header.www_crc32)
    {
        fclose(file);
        firmware_update_restore_factory("Written data verification failed");
        return;
    }
    fclose(file);

    err = config_clear_build_info();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to clear cached build info: %s", esp_err_to_name(err));
    }

    firmware_update_set_state(FW_UPDATE_REBOOTING);
    firmware_update_set_progress(100);
    firmware_update_set_message("Installation complete, rebooting");

    err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set factory as boot partition: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

void firmware_update_init(void)
{
    firmware_update_refresh_availability();

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == NULL)
    {
        ESP_LOGE(TAG, "Failed to determine running partition");
        return;
    }

    ESP_LOGI(TAG, "Running from partition '%s'", running->label);

    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
    {
        ESP_LOGI(TAG, "OTA staging boot detected, installing update");
        if (xTaskCreate(firmware_update_install_task, "fw_install", 8192, NULL, 2, NULL) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create install task, restoring factory boot");
            firmware_update_restore_factory("Failed to create install task");
        }

        /* The install task always ends with esp_restart(). Block app_main so the
           HTTP server never starts while the www partition is being rewritten. */
        vTaskSuspend(NULL);
    }
}

firmware_update_state_t firmware_update_get_state(void)
{
    return g_state;
}
