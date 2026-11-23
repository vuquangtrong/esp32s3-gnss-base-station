#include "storage.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"

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
        ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "Data partition size: total: %zu, used: %zu", total, used);
    }

    return ESP_OK;
}

char* storage_file_read(const char* filepath)
{
    if (filepath == NULL)
    {
        return NULL;
    }

    int fd = open(filepath, O_RDONLY, 0);
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

    char* buffer = malloc((size_t)st.st_size + 1);
    if (buffer == NULL)
    {
        close(fd);
        ESP_LOGE(TAG, "Failed to allocate memory for reading %s", filepath);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buffer, (size_t)st.st_size);
    close(fd);

    if (bytes_read != st.st_size)
    {
        free(buffer);
        return NULL;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}

static esp_err_t storage_file_save_json(const char* filepath, const cJSON* root)
{
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        cJSON_free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    close(fd);
    cJSON_free(json_str);

    return (written == (ssize_t)len) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_file_write_entry(const char* filepath, const char* serialized_object)
{
    if (filepath == NULL || serialized_object == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* entry = cJSON_Parse(serialized_object);
    if (!cJSON_IsObject(entry))
    {
        cJSON_Delete(entry);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = NULL;
    char* content = storage_file_read(filepath);
    if (content != NULL)
    {
        root = cJSON_Parse(content);
        free(content);
    }

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
        if (root == NULL)
        {
            cJSON_Delete(entry);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data))
    {
        cJSON_DeleteItemFromObject(root, "data");
        data = cJSON_AddArrayToObject(root, "data");
        if (data == NULL)
        {
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    if (!cJSON_AddItemToArray(data, entry))
    {
        cJSON_Delete(entry);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = storage_file_save_json(filepath, root);
    cJSON_Delete(root);
    return ret;
}

esp_err_t storage_file_delete_entry(const char* filepath, uint8_t index)
{
    if (filepath == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char* content = storage_file_read(filepath);
    if (content == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* root = cJSON_Parse(content);
    free(content);
    if (root == NULL)
    {
        return ESP_FAIL;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data) || (int)index >= cJSON_GetArraySize(data))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_DeleteItemFromArray(data, (int)index);

    esp_err_t ret = storage_file_save_json(filepath, root);
    cJSON_Delete(root);
    return ret;
}
