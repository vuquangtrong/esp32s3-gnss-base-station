#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_netif.h"

#define WIFI_AP_SSID_PREFIX "GNSS_"
#define WIFI_AP_PASSWORD    "12345678"
#define WIFI_STA_RETRY_MAX  10

#define MDNS_HOST_NAME     "gnss-station"
#define MDNS_INSTANCE_NAME "GNSS Station"

esp_err_t wifi_init(void);
esp_err_t wifi_sta_connect(const char* ssid, const char* password);

#endif  // WIFI_H
