#include "status.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char* TAG = "status";

static bool g_status_changed = true;

// Runtime status with fixed-size buffers,
// initialized with defaults
static status_entry_t g_status[STT_MAX] = {
    [STT_STA_STATUS] = {"sta_status", "0"},  // not connected
    [STT_STA_IP] = {"sta_ip", ""},
};

const char* status_name(status_type_t key)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return NULL;
    }
    return g_status[key].name;
}

const char* status_get(status_type_t key)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return NULL;
    }
    return g_status[key].value;
}

void status_set(status_type_t key, const char* value)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return;
    }

    if (value == NULL)
    {
        ESP_LOGE(TAG, "NULL value for status key: %d", key);
        return;
    }

    // Check if value actually changed
    if (strcmp(g_status[key].value, value) == 0)
    {
        return;
    }

    strncpy(g_status[key].value, value, STT_VALUE_LENGTH_MAX - 1);
    g_status[key].value[STT_VALUE_LENGTH_MAX - 1] = '\0';

    // Mark status as changed to regenerate JSON
    g_status_changed = true;
}

const char* status_get_all(void)
{
    static char* json_string = NULL;

    // Only regenerate JSON if status has changed
    if (!g_status_changed && json_string != NULL)
    {
        return json_string;
    }

    // Free previous JSON string
    if (json_string != NULL)
    {
        cJSON_free(json_string);
        json_string = NULL;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    for (int i = 0; i < STT_MAX; i++)
    {
        if (cJSON_AddStringToObject(root, g_status[i].name, g_status[i].value) == NULL)
        {
            ESP_LOGW(TAG, "Failed to add status %s to JSON", g_status[i].name);
        }
    }

    json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_string == NULL)
    {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return "{}";
    }

    // Clear the changed flag
    g_status_changed = false;

    return json_string;
}
