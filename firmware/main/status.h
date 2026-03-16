#ifndef ESP32S3_GNSS_STATUS_H
#define ESP32S3_GNSS_STATUS_H

#include <esp_err.h>

#define STATUS_LEN_MAX 128

typedef enum
{
    STATUS_START = 0,
    STATUS_GNSS_GGA = STATUS_START,
    STATUS_GNSS_GST,
    STATUS_GNSS_MODE,
    STATUS_NTRIP_CLI_STATUS,
    STATUS_NTRIP_CAS_STATUS,
    STATUS_WIFI_STATUS,
    STATUS_BATTERY,
    STATUS_MAX
} status_t;

esp_err_t status_init();
void status_set(status_t type, const char* value);
char* status_get(status_t type);

#endif  // ESP32S3_GNSS_STATUS_H
