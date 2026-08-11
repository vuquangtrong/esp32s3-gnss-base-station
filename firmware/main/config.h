#ifndef CONFIG_H
#define CONFIG_H

#include "esp_err.h"

#define CFG_VALUE_LENGTH_MAX (64)

typedef enum
{
    // Project
    CFG_PRJ_VERSION,
    CFG_BUILD_DATE,
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
const char* config_get(config_type_t key);
esp_err_t config_set(config_type_t key, const char* value);

const char* config_get_all(void);

#endif  // CONFIG_H
