#ifndef SERVER_H
#define SERVER_H

#include "esp_err.h"
#include "esp_vfs.h"

#define MDNS_HOST_NAME     "gnss-station"
#define MDNS_INSTANCE_NAME "GNSS Station 2.0"

#define WWW_FS_PARTITION_LABEL "www"
#define WWW_FS_BASE_PATH       "/www"

#define SERVER_FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SERVER_BUFFER_SIZE   (10240)

typedef struct
{
    char filepath[SERVER_FILE_PATH_MAX];
    char etag[64];
} etag_cache_entry_t;

typedef struct server_context
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char buffer[SERVER_BUFFER_SIZE];
} server_context_t;

esp_err_t server_init(void);

#endif  // SERVER_H
