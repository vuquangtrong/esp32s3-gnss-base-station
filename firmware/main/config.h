#ifndef CONFIG_H
#define CONFIG_H

#include "esp_err.h"

#define CFG_VALUE_LENGTH_MAX (64)

typedef enum
{
    CFG_PRJ_VERSION,
    CFG_BUILD_DATE,
    CFG_GIT_COMMIT,
    CFG_WIFI_SSID,
    CFG_WIFI_PASSWORD,
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
