#ifndef PPP_LOGGER_H
#define PPP_LOGGER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PPP_LOGGER_BUFFER_SIZE       (32 * 1024)
#define PPP_LOGGER_CHUNK_SIZE        512
#define PPP_LOGGER_FLUSH_INTERVAL_MS 10000
#define PPP_LOGGER_UART_TIMEOUT_MS   100
#define PPP_LOGGER_STOP_TIMEOUT_MS   2000
#define PPP_LOGGER_FILENAME_MAX      64

esp_err_t ppp_logger_init(void);
esp_err_t ppp_logger_start(const char* filename);
esp_err_t ppp_logger_stop(void);

#endif  // PPP_LOGGER_H
