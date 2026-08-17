#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#define DATA_PARTITION_LABEL "data"
#define DATA_BASE_PATH       "/data"

#define STORAGE_NTRIP_FILE "/data/ntrip.json"
#define STORAGE_BASE_FILE  "/data/base.json"
#define STORAGE_WIFI_FILE  "/data/wifi.json"

esp_err_t storage_init(void);
char* storage_read_json_file(const char* filepath);
esp_err_t storage_save_entry(const char* filepath, cJSON* new_item, const char* match_key);
esp_err_t storage_delete_entry(const char* filepath, int32_t index);

#endif  // STORAGE_H
