#ifndef ESP32S3_GNSS_UART_H
#define ESP32S3_GNSS_UART_H

#include <esp_err.h>
#include <esp_event.h>

extern esp_event_base_t const UART_RTCM3_EVENT_READ;
extern esp_event_base_t const UART_RTCM3_EVENT_WRITE;
extern esp_event_base_t const UART_STATUS_EVENT_READ;
extern esp_event_base_t const UART_STATUS_EVENT_WRITE;

esp_err_t uart_init();

void uart_register_handler(esp_event_base_t event_base, esp_event_handler_t event_handler);
void uart_unregister_handler(esp_event_base_t event_base, esp_event_handler_t event_handler);

void ubx_set_default();
void ubx_set_mode_rover();
void ubx_set_mode_survey(const char* dur, const char* acc);
void ubx_set_mode_fixed(const char* lat, const char* lon, const char* alt);
void ubx_write_rtcm3(const char* buffer, size_t len);

#endif  // ESP32S3_GNSS_UART_H
