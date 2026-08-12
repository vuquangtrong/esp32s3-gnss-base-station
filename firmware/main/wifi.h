#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t wifi_init(void);
esp_netif_t* wifi_get_ap_netif(void);
esp_netif_t* wifi_get_sta_netif(void);
esp_err_t wifi_sta_connect(const char* ssid, const char* password);

#endif  // WIFI_H
