#ifndef ESP32S3_GNSS_UBLOX_H
#define ESP32S3_GNSS_UBLOX_H

#include <stdint.h>

typedef enum
{
    GNSS_MODE_ROVER = 0,
    GNSS_MODE_SURVEY,
    GNSS_MODE_FIXED
} gnss_mode_t;

uint32_t ubx_gen_cmd(const char* msg, uint8_t* buff);

#endif  // ESP32S3_GNSS_UBLOX_H
