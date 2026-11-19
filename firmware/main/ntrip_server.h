#ifndef NTRIP_SERVER_H
#define NTRIP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NTRIP_SERVER_PORT        2101
#define NTRIP_SERVER_MAX_CLIENTS 4
#define NTRIP_SERVER_BUFFER_SIZE 2048
#define NTRIP_SERVER_MOUNTPOINT  "BASE"

// 0: negotiate from the client request, 1: always reply with NTRIP 1.0 (ICY), 2: always reply with NTRIP 2.0 (HTTP)
#define NTRIP_SERVER_FORCE_VERSION      0
#define NTRIP_SERVER_REQUEST_TIMEOUT_MS 500
#define NTRIP_SERVER_SEND_RETRY_MAX     200

#define NTRIP_SERVER_REQUEST_BUFFER_SIZE        1024
#define NTRIP_SERVER_DUMMY_RX_SIZE              128
#define NTRIP_SERVER_RECV_DRAIN_MAX             16
#define NTRIP_SERVER_SELECT_TIMEOUT_MS          20
#define NTRIP_SERVER_TASK_STACK_SIZE            8192
#define NTRIP_SERVER_TASK_PRIORITY              5
#define NTRIP_SERVER_TCP_KEEPALIVE_IDLE_SEC     30
#define NTRIP_SERVER_TCP_KEEPALIVE_INTERVAL_SEC 10
#define NTRIP_SERVER_TCP_KEEPALIVE_COUNT        3

typedef enum
{
    NTRIP_VERSION_1,
    NTRIP_VERSION_2
} ntrip_version_t;

esp_err_t ntrip_server_start(void);
esp_err_t ntrip_server_stop(void);

#endif  // NTRIP_SERVER_H
