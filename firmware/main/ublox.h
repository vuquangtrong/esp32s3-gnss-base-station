#ifndef UBLOX_H
#define UBLOX_H

#include <stdint.h>

uint32_t ubx_gen_cmd(const char* msg, uint8_t* buff);

#endif  // UBLOX_H
