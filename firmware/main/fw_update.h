#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <stdint.h>

#include "esp_err.h"

#define FW_UPDATE_FILENAME    "update.bin"
#define FW_UPDATE_MAGIC       "GNSSFWUPD"
#define FW_UPDATE_MAGIC_LEN   16
#define FW_UPDATE_VERSION     1
#define FW_UPDATE_HEADER_SIZE 4096
#define FW_UPDATE_BUFFER_SIZE (16 * 1024)
#define FW_UPDATE_SHA256_LEN  32

typedef struct __attribute__((packed))
{
    char magic[FW_UPDATE_MAGIC_LEN];
    uint32_t version;
    uint32_t app_size;
    uint8_t app_sha256[FW_UPDATE_SHA256_LEN];
    uint32_t www_size;
    uint8_t www_sha256[FW_UPDATE_SHA256_LEN];
} fw_update_header_t;

void fw_update_check(void);
esp_err_t fw_update_start(void);

const char* fw_update_get_active_app(void);
const char* fw_update_get_active_www(void);

#endif  // FW_UPDATE_H
