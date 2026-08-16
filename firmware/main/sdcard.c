#include "sdcard.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "status.h"

static const char* TAG = "sdcard";

static sdmmc_card_t* g_card = NULL;

static esp_err_t sdcard_mount(void)
{
    if (g_card != NULL)
    {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,  //
        .max_files = SDCARD_MAX_FILES,
        .allocation_unit_size = SDCARD_ALLOCATION_UNIT_SIZE
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.unaligned_multi_block_rw_max_chunk_size = 8;
    host.max_freq_khz = SDCARD_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = SDCARD_BUS_WIDTH;
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0 = SDCARD_PIN_D0;
    slot_config.d1 = SDCARD_PIN_D1;
    slot_config.d2 = SDCARD_PIN_D2;
    slot_config.d3 = SDCARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem at %s", SDCARD_MOUNT_POINT);
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &g_card);

    if (ret != ESP_OK)
    {
        g_card = NULL;
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s)", esp_err_to_name(ret));
        }
        status_set(STT_SDCARD_STATUS, SDCARD_ERROR);
        return ret;
    }

    ESP_LOGI(TAG, "Filesystem mounted successfully");
    sdmmc_card_print_info(stdout, g_card);
    status_set(STT_SDCARD_STATUS, SDCARD_MOUNTED);
    return ESP_OK;
}

static void sdcard_unmount(void)
{
    if (g_card == NULL)
    {
        return;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, g_card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to unmount: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "SD card unmounted");
    }

    g_card = NULL;
    status_set(STT_SDCARD_STATUS, SDCARD_REMOVED);
}

static void sdcard_task(void* arg)
{
    while (1)
    {
        if (g_card != NULL)
        {
            // Card was mounted — check if still present
            esp_err_t ret = sdmmc_get_status(g_card);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "SD card removed or not responding");
                sdcard_unmount();
            }
        }
        else
        {
            // Card not mounted — try to mount (detect insertion)
            esp_err_t ret = sdcard_mount();
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "SD card detected and mounted");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
    }
}

esp_err_t sdcard_init(void)
{
    // Attempt initial mount
    esp_err_t ret = sdcard_mount();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "No SD card at startup, monitor task will keep trying");
    }

    // Start monitor task to detect insert/remove
    if (xTaskCreate(sdcard_task, "sdcard", 2560, NULL, 1, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create SD card monitor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD card monitor task started");
    return ESP_OK;
}

esp_err_t sdcard_remount(void)
{
    ESP_LOGI(TAG, "Remounting SD card");
    sdcard_unmount();
    return sdcard_mount();
}

FILE* sdcard_open(const char* filename, const char* mode)
{
    if (filename == NULL || mode == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters: filename or mode is NULL");
        return NULL;
    }

    // Build full path with mount point
    char filepath[256] = {0};
    snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, filename);

    FILE* file = fopen(filepath, mode);

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return NULL;
    }

    ESP_LOGI(TAG, "File %s opened, mode: %s", filepath, mode);
    return file;
}

size_t sdcard_write(FILE* file, const void* data, size_t size)
{
    if (file == NULL || data == NULL)
    {
        ESP_LOGE(TAG, "Invalid parameters: file or data is NULL");
        return 0;
    }

    if (size == 0)
    {
        return 0;
    }

    size_t written = fwrite(data, 1, size, file);

    if (written != size)
    {
        ESP_LOGW(TAG, "Wrote %zu bytes, expected %zu bytes", written, size);
    }
    fflush(file);
    return written;
}

esp_err_t sdcard_close(FILE* file)
{
    if (file == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int ret = fclose(file);

    if (ret == 0)
    {
        ESP_LOGI(TAG, "File closed successfully");
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to close file");
        return ESP_FAIL;
    }
}

const char* sdcard_list_ubx_files(void)
{
    static char* s_ubx_files_json_str = NULL;

    // Free previous result
    if (s_ubx_files_json_str != NULL)
    {
        free(s_ubx_files_json_str);
        s_ubx_files_json_str = NULL;
    }

    cJSON* files_array = cJSON_CreateArray();
    if (files_array == NULL)
    {
        return "{\"files\":[]}";
    }

    if (g_card != NULL)
    {
        DIR* dir = opendir(SDCARD_MOUNT_POINT);
        if (dir != NULL)
        {
            struct dirent* entry = NULL;
            while ((entry = readdir(dir)) != NULL)
            {
                if (entry->d_type == DT_DIR)
                {
                    continue;
                }

                size_t name_len = strlen(entry->d_name);
                if (name_len <= 4 || strcasecmp(&entry->d_name[name_len - 4], ".ubx") != 0)
                {
                    continue;
                }

                // Get file size via stat
                char filepath[128];
                snprintf(filepath, sizeof(filepath), "%s/%s", SDCARD_MOUNT_POINT, entry->d_name);
                struct stat st = {0};
                int64_t file_size = 0;
                if (stat(filepath, &st) == 0)
                {
                    file_size = (int64_t)st.st_size;
                }

                cJSON* file_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(file_obj, "name", entry->d_name);
                cJSON_AddNumberToObject(file_obj, "size", (double)file_size);
                cJSON_AddItemToArray(files_array, file_obj);
            }
            closedir(dir);
        }
    }

    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "files", files_array);
    s_ubx_files_json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return (s_ubx_files_json_str != NULL) ? s_ubx_files_json_str : "{\"files\":[]}";
}
