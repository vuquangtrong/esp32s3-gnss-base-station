#ifndef SERVER_H
#define SERVER_H

#include <string.h>

#include "esp_err.h"
#include "esp_vfs.h"

#define MDNS_HOST_NAME     "gnss-station"
#define MDNS_INSTANCE_NAME "GNSS Station"

#define WWW_PARTITION_LABEL  "www"
#define WWW_BASE_PATH        "/www"
#define FILE_ETAG_LENGTH_MAX 64

#define SERVER_FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SERVER_BUFFER_SIZE   2048

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

typedef struct
{
    char filepath[SERVER_FILE_PATH_MAX];
    char etag[FILE_ETAG_LENGTH_MAX];
} etag_cache_entry_t;

esp_err_t server_init(void);

#endif  // SERVER_H
