#include "status.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#define STT_JSON_LENGTH_MAX 4096

static const char* TAG = "status";

static bool g_status_changed = true;
static char g_json[STT_JSON_LENGTH_MAX];

// Runtime status with fixed-size buffers,
// initialized with defaults
static status_entry_t g_status[STT_MAX] = {
    // Battery
    [STT_BAT_VOLT] = {.name = "bat_volt", .type = STT_VALUE_INT, .value = {.i_value = 0}},

    // WiFi
    [STT_WIFI_STATUS] = {.name = "wifi_status", .type = STT_VALUE_INT, .value = {.i_value = CONN_DISCONNECTED}},
    [STT_WIFI_IP_ADDR] = {.name = "wifi_ip_addr", .type = STT_VALUE_STRING, .value = {.str_value = ""}},

    // GNSS Mode
    [STT_GNSS_MODE] = {.name = "gnss_mode", .type = STT_VALUE_INT, .value = {.i_value = GNSS_ROVER}},

    // SDCard
    [STT_SDCARD_STATUS] = {.name = "sdcard_status", .type = STT_VALUE_INT, .value = {.i_value = SDCARD_REMOVED}},

    // Logger
    [STT_LOGGER_STATUS] = {.name = "logger_status", .type = STT_VALUE_INT, .value = {.i_value = LOGGER_STOPPED}},
    [STT_LOGGER_FILE] = {.name = "logger_file", .type = STT_VALUE_STRING, .value = {.str_value = ""}},
    [STT_LOGGER_SIZE] = {.name = "logger_size", .type = STT_VALUE_INT, .value = {.i_value = 0}},

    // NTRIP Client
    [STT_NTRIP_CLIENT_STATUS] = {.name = "ntrip_client_status", .type = STT_VALUE_INT, .value = {.i_value = CONN_DISCONNECTED}},
    [STT_NTRIP_RECEIVED_BYTES] = {.name = "ntrip_received_bytes", .type = STT_VALUE_INT, .value = {.i_value = 0}},

    // NTRIP Server
    [STT_NTRIP_SERVER_STATUS] = {.name = "ntrip_server_status", .type = STT_VALUE_INT, .value = {.i_value = CONN_DISCONNECTED}},
    [STT_NTRIP_CONN_COUNT] = {.name = "ntrip_conn_count", .type = STT_VALUE_INT, .value = {.i_value = 0}},
    [STT_NTRIP_SENT_BYTES] = {.name = "ntrip_sent_bytes", .type = STT_VALUE_INT, .value = {.i_value = 0}},

    // Firmware Update
    [STT_FW_UPDATE_AVAILABLE] = {.name = "fw_update_available", .type = STT_VALUE_INT, .value = {.i_value = 0}},
    [STT_FW_UPDATE_STATUS] = {.name = "fw_update_status", .type = STT_VALUE_INT, .value = {.i_value = FW_UPDATE_IDLE}},
};

static bool status_key_valid(status_type_t key)
{
    return (uint32_t)key < (uint32_t)STT_MAX;
}

const char* status_name(status_type_t key)
{
    if (!status_key_valid(key))
    {
        ESP_LOGE(TAG, "Invalid status key: %d", (int)key);
        return NULL;
    }
    return g_status[key].name;
}

status_value_type_t status_type(status_type_t key)
{
    if (!status_key_valid(key))
    {
        ESP_LOGE(TAG, "Invalid status key: %d", (int)key);
        return STT_VALUE_STRING;
    }
    return g_status[key].type;
}

void status_set_int(status_type_t key, int32_t value)
{
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_INT)
    {
        ESP_LOGE(TAG, "Invalid int status key: %d", (int)key);
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
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_DOUBLE)
    {
        ESP_LOGE(TAG, "Invalid double status key: %d", (int)key);
        return;
    }

    if (!isfinite(value))
    {
        ESP_LOGE(TAG, "Non-finite value for status key: %s", g_status[key].name);
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
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_STRING)
    {
        ESP_LOGE(TAG, "Invalid string status key: %d", (int)key);
        return;
    }

    if (value == NULL)
    {
        ESP_LOGE(TAG, "NULL value for status key: %s", g_status[key].name);
        return;
    }

    if (strcmp(g_status[key].value.str_value, value) != 0)
    {
        strlcpy(g_status[key].value.str_value, value, sizeof(g_status[key].value.str_value));
        g_status_changed = true;
    }
}

int32_t status_get_int(status_type_t key)
{
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_INT)
    {
        ESP_LOGE(TAG, "Invalid int status key: %d", (int)key);
        return 0;
    }

    return g_status[key].value.i_value;
}

double status_get_double(status_type_t key)
{
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_DOUBLE)
    {
        ESP_LOGE(TAG, "Invalid double status key: %d", (int)key);
        return 0.0;
    }

    return g_status[key].value.d_value;
}

const char* status_get_str(status_type_t key)
{
    if (!status_key_valid(key) || g_status[key].type != STT_VALUE_STRING)
    {
        ESP_LOGE(TAG, "Invalid string status key: %d", (int)key);
        return "";
    }

    return g_status[key].value.str_value;
}

const char* status_get_all(void)
{
    if (!g_status_changed && g_json[0] != '\0')
    {
        return g_json;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return "{}";
    }

    for (size_t i = 0; i < STT_MAX; i++)
    {
        cJSON* item = NULL;
        if (g_status[i].type == STT_VALUE_INT)
        {
            item = cJSON_AddNumberToObject(root, g_status[i].name, g_status[i].value.i_value);
        }
        else if (g_status[i].type == STT_VALUE_DOUBLE)
        {
            item = cJSON_AddNumberToObject(root, g_status[i].name, g_status[i].value.d_value);
        }
        else if (g_status[i].type == STT_VALUE_STRING)
        {
            item = cJSON_AddStringToObject(root, g_status[i].name, g_status[i].value.str_value);
        }

        if (item == NULL)
        {
            ESP_LOGE(TAG, "Failed to add status %s to JSON", g_status[i].name);
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

    g_status_changed = false;
    return g_json;
}
