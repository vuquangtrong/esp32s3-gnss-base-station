#include "status.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char* TAG = "status";

static bool g_status_changed = true;
static portMUX_TYPE g_status_spinlock = portMUX_INITIALIZER_UNLOCKED;

// Runtime status with fixed-size buffers,
// initialized with defaults
static status_entry_t g_status[STT_MAX] = {
    // WiFi Station
    [STT_STA_STATUS] = {.name = "sta_status", .type = STT_VALUE_INT, .value = {.i_value = WIFI_DISCONNECT}},
    [STT_STA_IP] = {.name = "sta_ip", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
    // GNSS Mode
    [STT_GNSS_MODE] = {.name = "gnss_mode", .type = STT_VALUE_INT, .value = {.i_value = GNSS_ROVER}},
    // GNSS Position
    [STT_GNSS_DATE] = {.name = "gnss_date", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
    [STT_GNSS_TIME] = {.name = "gnss_time", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
    [STT_GNSS_LAT] = {.name = "gnss_lat", .type = STT_VALUE_DOUBLE, .value = {.d_value = 0.0}},
    [STT_GNSS_LON] = {.name = "gnss_lon", .type = STT_VALUE_DOUBLE, .value = {.d_value = 0.0}},
    [STT_GNSS_ALT] = {.name = "gnss_alt", .type = STT_VALUE_DOUBLE, .value = {.d_value = 0.0}},
    [STT_GNSS_SAT] = {.name = "gnss_sat", .type = STT_VALUE_INT, .value = {.i_value = 0}},
    [STT_GNSS_FIX] = {.name = "gnss_fix", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
    [STT_GNSS_HACC] = {.name = "gnss_hacc", .type = STT_VALUE_DOUBLE, .value = {.d_value = 0.0}},
    [STT_GNSS_VACC] = {.name = "gnss_vacc", .type = STT_VALUE_DOUBLE, .value = {.d_value = 0.0}},
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

    portENTER_CRITICAL(&g_status_spinlock);
    if (g_status[key].value.i_value != value)
    {
        g_status[key].value.i_value = value;
        g_status_changed = true;
    }
    portEXIT_CRITICAL(&g_status_spinlock);
}

void status_set_double(status_type_t key, double value)
{
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return;
    }

    portENTER_CRITICAL(&g_status_spinlock);
    if (g_status[key].value.d_value != value)
    {
        g_status[key].value.d_value = value;
        g_status_changed = true;
    }
    portEXIT_CRITICAL(&g_status_spinlock);
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

    portENTER_CRITICAL(&g_status_spinlock);
    if (strcmp(g_status[key].value.str_value, value) != 0)
    {
        strncpy(g_status[key].value.str_value, value, STT_VALUE_LENGTH_MAX - 1);
        g_status[key].value.str_value[STT_VALUE_LENGTH_MAX - 1] = '\0';
        g_status_changed = true;
    }
    portEXIT_CRITICAL(&g_status_spinlock);
}

int status_get_int(status_type_t key)
{
    int val = 0;
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return 0;
    }

    portENTER_CRITICAL(&g_status_spinlock);
    val = g_status[key].value.i_value;
    portEXIT_CRITICAL(&g_status_spinlock);

    return val;
}

double status_get_double(status_type_t key)
{
    double val = 0.0;
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return 0.0;
    }

    portENTER_CRITICAL(&g_status_spinlock);
    val = g_status[key].value.d_value;
    portEXIT_CRITICAL(&g_status_spinlock);

    return val;
}

const char* status_get_str(status_type_t key)
{
    static char buf[STT_VALUE_LENGTH_MAX] = {0};
    if (key >= STT_MAX)
    {
        ESP_LOGE(TAG, "Invalid status key: %d", key);
        return "";
    }

    portENTER_CRITICAL(&g_status_spinlock);
    strncpy(buf, g_status[key].value.str_value, STT_VALUE_LENGTH_MAX - 1);
    buf[STT_VALUE_LENGTH_MAX - 1] = '\0';
    portEXIT_CRITICAL(&g_status_spinlock);

    return buf;
}

const char* status_get_all(void)
{
    static char* json_string = NULL;

    portENTER_CRITICAL(&g_status_spinlock);
    bool changed = g_status_changed;
    portEXIT_CRITICAL(&g_status_spinlock);

    // Only regenerate JSON if status has changed
    if (!changed && json_string != NULL)
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

    status_entry_t temp_status[STT_MAX];
    portENTER_CRITICAL(&g_status_spinlock);
    memcpy(temp_status, g_status, sizeof(g_status));
    g_status_changed = false;
    portEXIT_CRITICAL(&g_status_spinlock);

    for (int i = 0; i < STT_MAX; i++)
    {
        if (temp_status[i].type == STT_VALUE_INT)
        {
            if (cJSON_AddNumberToObject(root, temp_status[i].name, temp_status[i].value.i_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", temp_status[i].name);
            }
        }
        else if (temp_status[i].type == STT_VALUE_DOUBLE)
        {
            if (cJSON_AddNumberToObject(root, temp_status[i].name, temp_status[i].value.d_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", temp_status[i].name);
            }
        }
        else if (temp_status[i].type == STT_VALUE_STRING)
        {
            if (cJSON_AddStringToObject(root, temp_status[i].name, temp_status[i].value.str_value) == NULL)
            {
                ESP_LOGW(TAG, "Failed to add status %s to JSON", temp_status[i].name);
            }
        }
    }

    json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_string == NULL)
    {
        ESP_LOGE(TAG, "Failed to print JSON string");
        return "{}";
    }

    return json_string;
}
