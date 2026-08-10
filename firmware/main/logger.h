#ifndef LOGGER_H
#define LOGGER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "uart.h"

#define LOGGER_QUEUE_SIZE 32

esp_err_t logger_init(void);
esp_err_t logger_task_start(void);
esp_err_t logger_post_buf(uart_buf_t* buf);

#endif  // LOGGER_H
