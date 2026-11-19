#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define FIRMWARE_UPDATE_FILENAME      "update.bin"
#define FIRMWARE_PACKAGE_MAGIC        "GNSSFWPK"
#define FIRMWARE_PACKAGE_VERSION      1
#define FIRMWARE_PACKAGE_HEADER_SIZE  4096
#define FIRMWARE_UPDATE_BUFFER_SIZE   (16 * 1024)

typedef enum
{
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_CHECKING,
    FW_UPDATE_STAGING,
    FW_UPDATE_INSTALLING,
    FW_UPDATE_ERROR,
    FW_UPDATE_REBOOTING
} firmware_update_state_t;

typedef struct
{
    uint8_t magic[8];
    uint32_t format_version;
    uint32_t total_size;
    uint32_t app_offset;
    uint32_t app_size;
    uint32_t app_crc32;
    uint32_t www_offset;
    uint32_t www_size;
    uint32_t www_crc32;
} firmware_package_header_t;

void firmware_update_init(void);
void firmware_update_refresh_availability(void);
esp_err_t firmware_update_check(uint32_t* file_size);
esp_err_t firmware_update_start(void);
firmware_update_state_t firmware_update_get_state(void);

#endif  // FIRMWARE_UPDATE_H
