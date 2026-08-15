#ifndef LOGGER_H
#define LOGGER_H

#include "esp_err.h"

#define LOGGER_WRITE_BUFFER_SIZE (32 * 1024)
#define LOGGER_FLUSH_INTERVAL_MS 5000

esp_err_t logger_start(const char* filename);
esp_err_t logger_stop(void);

#endif  // LOGGER_H
