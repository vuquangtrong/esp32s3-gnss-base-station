#ifndef PARSER_H
#define PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NMEA_BUFFER_SIZE 128

#define UBX_SYNC1      0xB5
#define UBX_SYNC2      0x62
#define UBX_CLASS_NAV  0x01
#define UBX_ID_NAV_PVT 0x07

typedef struct
{
    char buf[NMEA_BUFFER_SIZE];
    uint16_t idx;
    bool in_msg;
} nmea_parser_ctx_t;

typedef struct __attribute__((packed))
{
    uint32_t iTOW;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t valid;
    uint32_t tAcc;
    int32_t nano;
    uint8_t fixType;
    uint8_t flags;
    uint8_t flags2;
    uint8_t numSV;
    int32_t lon;
    int32_t lat;
    int32_t height;
    int32_t hMSL;
    uint32_t hAcc;
    uint32_t vAcc;
    int32_t velN;
    int32_t velE;
    int32_t velD;
    int32_t gSpeed;
    int32_t headMot;
    uint32_t sAcc;
    uint32_t headAcc;
    uint16_t pDOP;
    uint8_t flags3;
    uint8_t reserved1[5];
    int32_t headVeh;
    int16_t magDec;
    uint16_t magAcc;
} ubx_nav_pvt_t;

typedef enum
{
    UBX_STATE_IDLE = 0,
    UBX_STATE_SYNC2,
    UBX_STATE_CLASS,
    UBX_STATE_ID,
    UBX_STATE_LEN_L,
    UBX_STATE_LEN_H,
    UBX_STATE_PAYLOAD,
    UBX_STATE_SKIP
} ubx_parser_state_t;

typedef struct
{
    ubx_parser_state_t state;
    uint8_t msg_class;
    uint8_t msg_id;
    uint16_t payload_len;
    uint16_t payload_idx;
    uint16_t skip_bytes_remaining;
    uint8_t payload[96];
} ubx_parser_ctx_t;

esp_err_t parser_init(void);

const char* parser_get_nmea_gga(void);

#endif  // PARSER_H
