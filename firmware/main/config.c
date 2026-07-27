#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "config";
static const char* NVS_NAMESPACE = "gnss_config";

static nvs_handle_t g_nvs_handle = 0;

// Runtime configuration storage with fixed-size buffers,
// initialized with defaults
static config_entry_t g_configs[CFG_MAX] = {
    [CFG_PRJ_VERSION] = {"prj_version", PROJECT_VERSION},
    [CFG_GIT_COMMIT] = {"git_commit", GIT_COMMIT},
};

esp_err_t config_init(void)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Initializing Configuration");

    // Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing it");
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase NVS flash: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize NVS flash after erase: %s", esp_err_to_name(err));
            return err;
        }
    }

    // Open NVS namespace
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "NVS namespace '%s' opened successfully", NVS_NAMESPACE);

    // Load configuration from NVS, keep defaults if not found
    for (int i = 0; i < CFG_MAX; i++)
    {
        char buffer[CFG_VALUE_LENGTH_MAX] = {0};
        size_t length = sizeof(buffer);

        err = nvs_get_str(g_nvs_handle, g_configs[i].name, buffer, &length);
        if (err == ESP_OK)
        {
            // Value exists in NVS, copy from buffer
            strncpy(g_configs[i].value, buffer, length - 1);
            g_configs[i].value[length - 1] = '\0';
            ESP_LOGI(TAG, "Loaded %s from NVS: %s", g_configs[i].name, g_configs[i].value);
        }
        else if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            // Value doesn't exist in NVS, keep default
            ESP_LOGI(TAG, "Using default for %s: %s", g_configs[i].name, g_configs[i].value);
        }
        else
        {
            ESP_LOGW(TAG, "Error reading %s from NVS: %s, using default", g_configs[i].name, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Configuration initialized successfully");

    return ESP_OK;
}

const char* config_get(config_type_t key)
{
    if (key < CFG_MAX)
    {
        return g_configs[key].value;
    }
    ESP_LOGW(TAG, "Invalid config key: %d", key);
    return NULL;
}

esp_err_t config_set(config_type_t key, const char* value)
{
    if (key >= CFG_MAX)
    {
        ESP_LOGW(TAG, "Invalid config key: %d", key);
        return ESP_ERR_INVALID_ARG;
    }

    if (value == NULL)
    {
        ESP_LOGW(TAG, "Null value for config key: %d", key);
        return ESP_ERR_INVALID_ARG;
    }

    // Validate value length
    if (strlen(value) >= CFG_VALUE_LENGTH_MAX)
    {
        ESP_LOGW(TAG, "Config value too long (max %d bytes): %s", CFG_VALUE_LENGTH_MAX - 1, g_configs[key].name);
        return ESP_ERR_INVALID_ARG;
    }

    // Update in memory (safe copy into fixed buffer)
    strncpy(g_configs[key].value, value, CFG_VALUE_LENGTH_MAX - 1);
    g_configs[key].value[CFG_VALUE_LENGTH_MAX - 1] = '\0';

    // Write to NVS
    esp_err_t err = nvs_set_str(g_nvs_handle, g_configs[key].name, g_configs[key].value);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set %s in NVS: %s", g_configs[key].name, esp_err_to_name(err));
        return err;
    }

    // Commit to NVS
    err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit NVS after setting %s: %s", g_configs[key].name, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Config %s set to: %s", g_configs[key].name, g_configs[key].value);
    return ESP_OK;
}
