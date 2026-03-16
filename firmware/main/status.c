#include "status.h"

#include <string.h>

#include "config.h"
#include "util.h"

static const char* TAG = "STATUS";

// ordered status list
static char status[STATUS_MAX][STATUS_LEN_MAX];
static char status_name[STATUS_MAX][STATUS_LEN_MAX / 2] = {
    "gnss_gga",          //
    "gnss_gst",          //
    "gnss_mode",         //
    "ntrip_cli_status",  //
    "ntrip_cas_status",  //
    "wifi_status",       //
    "battery",           //
};

esp_err_t status_init()
{
    // clear allocated memory
    memset(status, 0, STATUS_MAX * STATUS_LEN_MAX);

    return ESP_OK;
}

void status_set(status_t type, const char* value)
{
    memset(status[type], 0, STATUS_LEN_MAX);
    strncpy(status[type], value, STATUS_LEN_MAX);
    ESP_LOGD(TAG, "Status set:\r\nkey=%s\r\nval=%s", status_name[type], status[type]);
}

char* status_get(status_t type)
{
    ESP_LOGD(TAG, "Status get:\r\nkey=%s\r\nval=%s", status_name[type], status[type]);
    return status[type];
}
