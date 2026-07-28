#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"

esp_err_t wifi_init(void);
esp_err_t wifi_sta_connect(const char* ssid, const char* password);

#endif  // WIFI_H
