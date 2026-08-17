#ifndef NTRIP_SERVER_H
#define NTRIP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NTRIP_SERVER_PORT        2101
#define NTRIP_SERVER_MAX_CLIENTS 4
#define NTRIP_SERVER_BUFFER_SIZE 1024
#define NTRIP_SERVER_MOUNTPOINT  "BASE"

esp_err_t ntrip_server_start(void);
esp_err_t ntrip_server_stop(void);

#endif  // NTRIP_SERVER_H
