#ifndef NTRIP_CLIENT_H
#define NTRIP_CLIENT_H

#include <stdint.h>

#include "esp_err.h"

#define NTRIP_HOST_LEN_MAX       64
#define NTRIP_USER_LEN_MAX       64
#define NTRIP_PASS_LEN_MAX       64
#define NTRIP_MOUNTPOINTS_MAX    32
#define NTRIP_MOUNTPOINT_LEN_MAX 64
#define NTRIP_BUFFER_SIZE        1024
#define NMEA_BUFFER_SIZE         128

typedef struct
{
    char host[NTRIP_HOST_LEN_MAX];
    uint16_t port;
    char mountpoint[NTRIP_MOUNTPOINT_LEN_MAX];
    char username[NTRIP_USER_LEN_MAX];
    char password[NTRIP_PASS_LEN_MAX];
} ntrip_client_args_t;

void ntrip_client_clear_mountpoints(void);
void ntrip_client_add_mountpoint(const char* mountpoint);
int ntrip_client_get_mountpoint_count(void);
const char* ntrip_client_get_mountpoint(int index);

void ntrip_client_get_mountpoints(const char* host, uint16_t port, const char* username, const char* password);
void ntrip_client_connect_stream(const char* host, uint16_t port, const char* mountpoint, const char* username, const char* password);
void ntrip_client_set_gga(const char* gga);
void ntrip_client_disconnect_stream(void);

#endif  // NTRIP_CLIENT_H
