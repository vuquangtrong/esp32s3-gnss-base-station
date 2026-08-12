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
    // Battery
    [STT_BAT_VOLT] = {.name = "bat_volt", .type = STT_VALUE_INT, .value = {.i_value = 0}},
    // WiFi
    [STT_WIFI_STATUS] = {.name = "wifi_status", .type = STT_VALUE_INT, .value = {.i_value = CONN_DISCONNECT}},
    [STT_WIFI_IP_ADDR] = {.name = "wifi_ip_addr", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
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

status_value_type_t status_type(status_type_t key)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return STT_VALUE_STRING;
    }
    return g_status[key].type;
}

void status_set_int(status_type_t key, int value)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return;
    }

    if (g_status[key].value.i_value != value)
    {
        g_status[key].value.i_value = value;
        g_status_changed = true;
    }
}

void status_set_double(status_type_t key, double value)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return;
    }

    if (g_status[key].value.d_value != value)
    {
        g_status[key].value.d_value = value;
        g_status_changed = true;
    }
}

void status_set_str(status_type_t key, const char* value)
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

    if (strcmp(g_status[key].value.str_value, value) != 0)
    {
        snprintf(g_status[key].value.str_value, STT_VALUE_LENGTH_MAX, "%s", value);
        g_status_changed = true;
    }
}

int status_get_int(status_type_t key)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return 0;
    }

    return g_status[key].value.i_value;
}

double status_get_double(status_type_t key)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return 0.0;
    }

    return g_status[key].value.d_value;
}

const char* status_get_str(status_type_t key)
{
    static char buf[STT_VALUE_LENGTH_MAX] = {0};
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return "";
    }

    snprintf(buf, STT_VALUE_LENGTH_MAX, "%s", g_status[key].value.str_value);
    return buf;
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

    // Create a new JSON object
    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    // Add all status entries to the JSON object
    for (int i = 0; i < STT_MAX; i++)
    {
        if (g_status[i].type == STT_VALUE_INT)
        {
            if (cJSON_AddNumberToObject(root, g_status[i].name, g_status[i].value.i_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", g_status[i].name);
            }
        }
        else if (g_status[i].type == STT_VALUE_DOUBLE)
        {
            if (cJSON_AddNumberToObject(root, g_status[i].name, g_status[i].value.d_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", g_status[i].name);
            }
        }
        else if (g_status[i].type == STT_VALUE_STRING)
        {
            if (cJSON_AddStringToObject(root, g_status[i].name, g_status[i].value.str_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", g_status[i].name);
            }
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

    g_status_changed = false;
    return json_string;
}
