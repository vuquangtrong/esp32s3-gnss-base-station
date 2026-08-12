#include "config.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "config";
static const char* NVS_NAMESPACE = "gnss_config";

static nvs_handle_t g_nvs_handle = 0;
static bool g_config_changed = true;

// Runtime configuration storage with fixed-size buffers,
// initialized with defaults
static config_entry_t g_configs[CFG_MAX] = {
    // Project
    [CFG_VERSION] = {"version", VERSION},           //
    [CFG_BUILD_TIME] = {"build_time", BUILD_TIME},  //
    [CFG_GIT_COMMIT] = {"git_commit", GIT_COMMIT},  //
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
            snprintf(g_configs[i].value, length, "%s", buffer);
            ESP_LOGI(TAG, "Loaded from NVS %s: %s", g_configs[i].name, g_configs[i].value);
        }
        else
        {
            // Value doesn't exist in NVS, keep default
            ESP_LOGI(TAG, "Using default for %s: %s", g_configs[i].name, g_configs[i].value);
        }
    }

    ESP_LOGI(TAG, "Configuration initialized successfully");

    return ESP_OK;
}

const char* config_name(config_type_t key)
{
    if (key >= CFG_MAX)
    {
        ESP_LOGW(TAG, "Invalid config key: %d", key);
        return NULL;
    }
    return g_configs[key].name;
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

    // Check if value actually changed
    if (strcmp(g_configs[key].value, value) == 0)
    {
        ESP_LOGD(TAG, "Config %s unchanged, skipping update", g_configs[key].name);
        return ESP_OK;
    }

    // Update in memory
    snprintf(g_configs[key].value, CFG_VALUE_LENGTH_MAX, "%s", value);

    // Mark config as changed to regenerate JSON
    g_config_changed = true;

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

const char* config_get(config_type_t key)
{
    if (key >= CFG_MAX)
    {
        ESP_LOGW(TAG, "Invalid config key: %d", key);
        return NULL;
    }
    return g_configs[key].value;
}

const char* config_get_all(void)
{
    static char* json_string = NULL;

    // Only regenerate JSON if config has changed
    if (!g_config_changed && json_string != NULL)
    {
        return json_string;
    }

    // Free previous JSON string
    if (json_string != NULL)
    {
        cJSON_free(json_string);
        json_string = NULL;
    }

    // Create a new JSON object
    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    // Add all config entries to the JSON object
    for (int i = 0; i < CFG_MAX; i++)
    {
        if (cJSON_AddStringToObject(root, g_configs[i].name, g_configs[i].value) == NULL)
        {
            ESP_LOGW(TAG, "Failed to add config %s to JSON", g_configs[i].name);
        }
    }

    // Convert JSON object to string
    json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_string == NULL)
    {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return "{}";
    }

    // Clear the changed flag
    g_config_changed = false;

    return json_string;
}
