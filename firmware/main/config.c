#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "build_info.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CFG_JSON_LENGTH_MAX 4096

static const char* TAG = "config";
static const char* NVS_NAMESPACE = "gnss_config";

static nvs_handle_t g_nvs_handle = 0;
static bool g_config_initialized = false;
static char g_json[CFG_JSON_LENGTH_MAX];

// Runtime configuration storage with fixed-size buffers,
// initialized with defaults
static config_entry_t g_configs[CFG_MAX] = {
    // Project
    [CFG_VERSION] = {"app_version", APP_VERSION, true},
    [CFG_BUILD_TIME] = {"build_time", BUILD_TIME, true},
    [CFG_GIT_COMMIT] = {"git_commit", GIT_COMMIT, true},
    // Device
    [CFG_DEVICE_ID] = {"device_id", ""},
    // WiFi
    [CFG_WIFI_SSID] = {"wifi_ssid", ""},
    [CFG_WIFI_PASSWORD] = {"wifi_password", ""},
    // NTRIP Client
    [CFG_NTRIP_SERVER] = {"ntrip_server", "14.238.1.125"},
    [CFG_NTRIP_PORT] = {"ntrip_port", "2101"},
    [CFG_NTRIP_MOUNTPOINT] = {"ntrip_mntpoint", "VRS.105M6"},
    [CFG_NTRIP_USERNAME] = {"ntrip_username", ""},
    [CFG_NTRIP_PASSWORD] = {"ntrip_password", ""},
    // Base Position
    [CFG_BASE_LAT] = {"base_lat", "21.028511234"},   // deg, 9 decimal places
    [CFG_BASE_LON] = {"base_lon", "105.804817890"},  // deg, 9 decimal places
    [CFG_BASE_HEIGHT] = {"base_height", "12.3456"},  // m, 4 decimal places
    // PPP Parameters
    [CFG_PPP_MIN_DUR] = {"ppp_min_dur", "600"},      // seconds
    [CFG_PPP_ACC_LIMIT] = {"ppp_acc_limit", "100"},  // mm
};

static bool config_key_valid(config_type_t key)
{
    return (uint32_t)key < (uint32_t)CFG_MAX;
}

esp_err_t config_init(void)
{
    if (g_config_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
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
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < CFG_MAX; i++)
    {
        if (g_configs[i].readonly)
        {
            continue;
        }

        size_t length = sizeof(g_configs[i].value);
        err = nvs_get_str(g_nvs_handle, g_configs[i].name, g_configs[i].value, &length);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Loaded %s from NVS", g_configs[i].name);
        }
        else if (err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGW(TAG, "Failed to load %s (%s), using default", g_configs[i].name, esp_err_to_name(err));
        }
    }

    g_config_initialized = true;
    ESP_LOGI(TAG, "Configuration initialized");

    return ESP_OK;
}

const char* config_name(config_type_t key)
{
    if (!config_key_valid(key))
    {
        ESP_LOGW(TAG, "Invalid config key: %d", (int)key);
        return NULL;
    }
    return g_configs[key].name;
}

esp_err_t config_set(config_type_t key, const char* value)
{
    if (!config_key_valid(key))
    {
        ESP_LOGW(TAG, "Invalid config key: %d", (int)key);
        return ESP_ERR_INVALID_ARG;
    }

    if (g_configs[key].readonly)
    {
        ESP_LOGW(TAG, "Read-only config key: %s", g_configs[key].name);
        return ESP_ERR_INVALID_ARG;
    }

    if (value == NULL)
    {
        ESP_LOGW(TAG, "Null value for config key: %s", g_configs[key].name);
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(value) >= sizeof(g_configs[key].value))
    {
        ESP_LOGW(TAG, "Config value too long for %s (max %zu bytes)", g_configs[key].name, sizeof(g_configs[key].value) - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_config_initialized)
    {
        ESP_LOGE(TAG, "Configuration is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(g_configs[key].value, value) == 0)
    {
        return ESP_OK;
    }

    esp_err_t err = nvs_set_str(g_nvs_handle, g_configs[key].name, value);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set %s in NVS: %s", g_configs[key].name, esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(g_nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit NVS after setting %s: %s", g_configs[key].name, esp_err_to_name(err));
        return err;
    }

    strlcpy(g_configs[key].value, value, sizeof(g_configs[key].value));
    ESP_LOGI(TAG, "Config %s updated", g_configs[key].name);

    return ESP_OK;
}

const char* config_get(config_type_t key)
{
    if (!config_key_valid(key))
    {
        ESP_LOGW(TAG, "Invalid config key: %d", (int)key);
        return NULL;
    }
    return g_configs[key].value;
}

const char* config_get_all(void)
{
    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    for (size_t i = 0; i < CFG_MAX; i++)
    {
        if (cJSON_AddStringToObject(root, g_configs[i].name, g_configs[i].value) == NULL)
        {
            ESP_LOGE(TAG, "Failed to add config %s to JSON", g_configs[i].name);
            cJSON_Delete(root);
            return "{}";
        }
    }

    bool printed = cJSON_PrintPreallocated(root, g_json, (int)sizeof(g_json), false);
    cJSON_Delete(root);

    if (!printed)
    {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return "{}";
    }

    return g_json;
}
