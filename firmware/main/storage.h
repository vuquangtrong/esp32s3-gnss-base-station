#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define DATA_PARTITION_LABEL "data"
#define DATA_BASE_PATH       "/data"

#define STORAGE_WIFI_FILE  "/data/wifi.json"
#define STORAGE_NTRIP_FILE "/data/ntrip.json"
#define STORAGE_BASE_FILE  "/data/base.json"

esp_err_t storage_init(void);

char* storage_read_json_file(const char* filepath);

esp_err_t storage_set_key(const char* filepath, const char* key, const cJSON* value);
esp_err_t storage_delete_key(const char* filepath, const char* key);

#endif  // STORAGE_H
