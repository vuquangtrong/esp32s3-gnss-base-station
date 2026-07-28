#include "status.h"

#include <string.h>

#include "esp_log.h"

static const char* TAG = "status";

// Runtime status with fixed-size buffers,
// initialized with defaults
static status_entry_t g_status[STT_MAX] = {
    [STT_STA_CONNECTED] = {"sta_connected", "0"},  // not connected
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
