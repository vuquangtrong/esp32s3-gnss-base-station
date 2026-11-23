#ifndef SERVER_H
#define SERVER_H

#include "esp_err.h"

#define WWW_BASE_PATH          "/www"
#define SERVER_FILE_PATH_MAX   256
#define SERVER_FILE_BUF_LEN    4096
#define SERVER_UART_BUF_LEN    1024
#define SERVER_WS_CLIENTS_MAX  16
#define SERVER_UART_TIMEOUT_MS 100

esp_err_t server_init(void);

#endif  // SERVER_H
