#include "status.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "status";

// Runtime status with fixed-size buffers,
// initialized with defaults
static status_entry_t g_status[STT_MAX] = {
    [STT_STA_STATUS] = {"sta_status", "0"},  // not connected
};

const char* status_name(status_type_t key)
{
    if (key < 0 || key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return NULL;
    }
    return g_status[key].name;
}

const char* status_get(status_type_t key)
{
    if (key < 0 || key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return NULL;
    }
    return g_status[key].value;
}

void status_set(status_type_t key, const char* value)
{
    if (key < 0 || key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return;
    }
    strncpy(g_status[key].value, value, STT_VALUE_LENGTH_MAX - 1);
    g_status[key].value[STT_VALUE_LENGTH_MAX - 1] = '\0';  // Ensure null-termination
}

const char* status_get_all(void)
{
    static char status_json[STT_MAX * (STT_VALUE_LENGTH_MAX + 32)];  // Adjust size as needed
    size_t offset = 0;

    offset += snprintf(status_json + offset, sizeof(status_json) - offset, "{");
    for (int i = 0; i < STT_MAX; i++)
    {
        if (i > 0)
        {
            offset += snprintf(status_json + offset, sizeof(status_json) - offset, ", ");
        }
        offset += snprintf(status_json + offset, sizeof(status_json) - offset, "\"%s\": \"%s\"", g_status[i].name, g_status[i].value);
    }
    snprintf(status_json + offset, sizeof(status_json) - offset, "}");

    return status_json;
}
