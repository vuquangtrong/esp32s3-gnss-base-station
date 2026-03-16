#ifndef ESP32S3_GNSS_WIFI_H
#define ESP32S3_GNSS_WIFI_H

#include <esp_err.h>
#include <stdbool.h>

#define WIFI_TRIAL_RESET true
#define WIFI_TRIAL_MAX   5

esp_err_t wifi_init();
esp_err_t wifi_connect(bool reset_trial);
esp_err_t wifi_disconnect();
void wait_for_ip();

#endif  // ESP32S3_GNSS_WIFI_H
