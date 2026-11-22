#ifndef UBLOX_H
#define UBLOX_H

#include <stddef.h>
#include <stdint.h>

uint32_t ubx_gen_cmd(const char* msg, uint8_t* buff, size_t buff_size);

#endif  // UBLOX_H
