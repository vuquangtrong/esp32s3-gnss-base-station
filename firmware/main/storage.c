#include "storage.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"

static const char* TAG = "storage";

esp_err_t storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = DATA_BASE_PATH,
        .partition_label = DATA_PARTITION_LABEL,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format LittleFS data partition");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS data partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS data (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Data partition size: total: %zu, used: %zu", total, used);
    }

    return ESP_OK;
}

char* storage_read_json_file(const char* filepath)
{
    if (filepath == NULL)
    {
        return NULL;
    }

    int32_t fd = open(filepath, O_RDONLY, 0);
    if (fd == -1)
    {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0)
    {
        close(fd);
        return NULL;
    }

    char* buffer = malloc(st.st_size + 1);
    if (buffer == NULL)
    {
        close(fd);
        ESP_LOGE(TAG, "Failed to allocate memory for reading %s", filepath);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buffer, st.st_size);
    close(fd);

    if (bytes_read <= 0)
    {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}

static esp_err_t storage_write_string_to_file(const char* filepath, const char* content)
{
    if (filepath == NULL || content == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1)
    {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return ESP_FAIL;
    }

    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);

    if (written != (ssize_t)len)
    {
        ESP_LOGE(TAG, "Failed to write all bytes to %s", filepath);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_save_entry(const char* filepath, cJSON* new_item, const char* match_key)
{
    if (filepath == NULL || new_item == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char* file_content = storage_read_json_file(filepath);
    cJSON* root_arr = NULL;
    if (file_content != NULL)
    {
        root_arr = cJSON_Parse(file_content);
        free(file_content);
    }

    if (root_arr == NULL || !cJSON_IsArray(root_arr))
    {
        if (root_arr != NULL)
        {
            cJSON_Delete(root_arr);
        }
        root_arr = cJSON_CreateArray();
        if (root_arr == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    bool updated = false;
    if (match_key != NULL)
    {
        cJSON* new_key_item = cJSON_GetObjectItem(new_item, match_key);
        if (new_key_item != NULL && cJSON_IsString(new_key_item))
        {
            int32_t count = cJSON_GetArraySize(root_arr);
            for (int32_t i = 0; i < count; i++)
            {
                cJSON* existing = cJSON_GetArrayItem(root_arr, i);
                cJSON* exist_key = cJSON_GetObjectItem(existing, match_key);
                if (exist_key != NULL && cJSON_IsString(exist_key) && strcmp(exist_key->valuestring, new_key_item->valuestring) == 0)
                {
                    cJSON_ReplaceItemInArray(root_arr, i, cJSON_Duplicate(new_item, 1));
                    updated = true;
                    break;
                }
            }
        }
    }

    if (!updated)
    {
        cJSON_AddItemToArray(root_arr, cJSON_Duplicate(new_item, 1));
    }

    char* json_str = cJSON_PrintUnformatted(root_arr);
    cJSON_Delete(root_arr);

    if (json_str == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_write_string_to_file(filepath, json_str);
    cJSON_free(json_str);

    return err;
}

esp_err_t storage_delete_entry(const char* filepath, int32_t index)
{
    if (filepath == NULL || index < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char* file_content = storage_read_json_file(filepath);
    if (file_content == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* root_arr = cJSON_Parse(file_content);
    free(file_content);

    if (root_arr == NULL || !cJSON_IsArray(root_arr))
    {
        if (root_arr != NULL)
        {
            cJSON_Delete(root_arr);
        }
        return ESP_FAIL;
    }

    int32_t size = cJSON_GetArraySize(root_arr);
    if (index >= size)
    {
        cJSON_Delete(root_arr);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_DeleteItemFromArray(root_arr, index);

    char* json_str = cJSON_PrintUnformatted(root_arr);
    cJSON_Delete(root_arr);

    if (json_str == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_write_string_to_file(filepath, json_str);
    cJSON_free(json_str);

    return err;
}
