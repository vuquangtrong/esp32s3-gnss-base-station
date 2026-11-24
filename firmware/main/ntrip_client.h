#ifndef NTRIP_CLIENT_H
#define NTRIP_CLIENT_H

#include <stdint.h>

#include "esp_err.h"

#define NTRIP_SERVER_LEN_MAX   64
#define NTRIP_MNTPOINT_LEN_MAX 64
#define NTRIP_USERNAME_LEN_MAX 64
#define NTRIP_PASSWORD_LEN_MAX 64
#define NTRIP_MNTPOINTS_MAX    64

#define NTRIP_NMEA_BUF_SIZE 128
#define NTRIP_LINE_BUF_SIZE 256
#define NTRIP_REQ_BUF_SIZE  512
#define NTRIP_RX_BUF_SIZE   1024
#define NTRIP_JSON_BUF_SIZE 2048

#define NTRIP_HTTP_TIMEOUT_MS            10000
#define NTRIP_STREAM_READ_TIMEOUT_MS     100
#define NTRIP_STREAM_MAX_IDLE_TICKS      300
#define NTRIP_STREAM_GGA_KEEPALIVE_TICKS 100
#define NTRIP_UART_READ_TIMEOUT_MS       100

typedef struct
{
    char server[NTRIP_SERVER_LEN_MAX];
    uint16_t port;
    char mntpoint[NTRIP_MNTPOINT_LEN_MAX];
    char username[NTRIP_USERNAME_LEN_MAX];
    char password[NTRIP_PASSWORD_LEN_MAX];
} ntrip_client_cfg_t;

esp_err_t ntrip_client_start_gga_task(void);
void ntrip_client_stop_gga_task(void);

const char* ntrip_client_query_mountpoints(const char* server, uint16_t port, const char* username, const char* password);

esp_err_t ntrip_client_connect(const char* server, uint16_t port, const char* mntpoint, const char* username, const char* password);
void ntrip_client_disconnect(void);

#endif  // NTRIP_CLIENT_H
