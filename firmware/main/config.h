#ifndef CONFIG_H
#define CONFIG_H

#include "esp_err.h"

#define CFG_VALUE_LENGTH_MAX 64

typedef enum
{
    // Project
    CFG_VERSION,
    CFG_BUILD_TIME,
    CFG_GIT_COMMIT,

    // WiFi
    CFG_WIFI_SSID,
    CFG_WIFI_PASSWORD,

    // NTRIP Client
    CFG_NTRIP_SERVER,
    CFG_NTRIP_PORT,
    CFG_NTRIP_MOUNTPOINT,
    CFG_NTRIP_USERNAME,
    CFG_NTRIP_PASSWORD,

    // Base Position
    CFG_BASE_LAT,     // deg, 9 decimal places
    CFG_BASE_LON,     // deg, 9 decimal places
    CFG_BASE_HEIGHT,  // m, 4 decimal places

    // PPP Parameters
    CFG_PPP_MIN_DUR,    // seconds
    CFG_PPP_ACC_LIMIT,  // mm

    //
    CFG_MAX
} config_type_t;

typedef struct
{
    const char* name;
    char value[CFG_VALUE_LENGTH_MAX];
} config_entry_t;

esp_err_t config_init(void);
const char* config_name(config_type_t key);
esp_err_t config_set(config_type_t key, const char* value);
const char* config_get(config_type_t key);
const char* config_get_all(void);

#endif  // CONFIG_H
