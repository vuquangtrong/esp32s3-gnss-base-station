#ifndef ESP32S3_GNSS_NTRIP_CLIENT_H
#define ESP32S3_GNSS_NTRIP_CLIENT_H

#include <esp_err.h>

esp_err_t ntrip_client_init();
char* ntrip_client_source_table();
void ntrip_client_get_mnts();
void ntrip_client_connect();
void ntrip_client_disconnect();

#endif  // ESP32S3_GNSS_NTRIP_CLIENT_H
