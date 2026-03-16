#ifndef ESP32S3_GNSS_CONFIG_H
#define ESP32S3_GNSS_CONFIG_H

#include <esp_err.h>

#define CONFIG_LEN_MAX 128

typedef enum
{
    CONFIG_START,
    CONFIG_HOSTNAME = CONFIG_START,
    CONFIG_VERSION,
    CONFIG_NVS_START,
    CONFIG_WIFI_SSID = CONFIG_NVS_START,
    CONFIG_WIFI_PWD,
    CONFIG_NTRIP_IP,
    CONFIG_NTRIP_PORT,
    CONFIG_NTRIP_USER,
    CONFIG_NTRIP_PWD,
    CONFIG_NTRIP_MNT,
    CONFIG_BASE_LAT,
    CONFIG_BASE_LON,
    CONFIG_BASE_ALT,
    CONFIG_MAX
} config_t;

esp_err_t config_init();
void config_set(config_t type, const char* value);
char* config_get(config_t type);
void config_reset();

#endif  // ESP32S3_GNSS_CONFIG_H
