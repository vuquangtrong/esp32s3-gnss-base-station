#include "storage.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

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

char* storage_read_json_file(const char* filepath)
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

static esp_err_t storage_write_json(const char* filepath, const cJSON* json)
{
    if (filepath == NULL || json == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char* json_str = cJSON_PrintUnformatted(json);
    if (json_str == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1)
    {
        cJSON_free(json_str);
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    close(fd);
    cJSON_free(json_str);

    if (written != (ssize_t)len)
    {
        ESP_LOGE(TAG, "Failed to write all bytes to %s", filepath);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_set_key(const char* filepath, const char* key, const cJSON* value)
{
    if (filepath == NULL || key == NULL || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = NULL;
    char* file_content = storage_read_json_file(filepath);
    if (file_content != NULL)
    {
        root = cJSON_Parse(file_content);
        free(file_content);
    }

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
        if (root == NULL)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON* copy = cJSON_Duplicate(value, 1);
    if (copy == NULL)
    {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    if (cJSON_HasObjectItem(root, key))
    {
        cJSON_ReplaceItemInObjectCaseSensitive(root, key, copy);
    }
    else
    {
        cJSON_AddItemToObject(root, key, copy);
    }

    esp_err_t err = storage_write_json(filepath, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t storage_delete_key(const char* filepath, const char* key)
{
    if (filepath == NULL || key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char* file_content = storage_read_json_file(filepath);
    if (file_content == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* root = cJSON_Parse(file_content);
    free(file_content);

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (!cJSON_HasObjectItem(root, key))
    {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);

    esp_err_t err = storage_write_json(filepath, root);
    cJSON_Delete(root);
    return err;
}
