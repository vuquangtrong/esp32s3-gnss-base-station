#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DATA_PARTITION_LABEL "data"
#define DATA_BASE_PATH       "/data"

#define STORAGE_WIFI_FILE  "/data/wifi.json"
#define STORAGE_NTRIP_FILE "/data/ntrip.json"
#define STORAGE_BASE_FILE  "/data/base.json"

esp_err_t storage_init(void);

char* storage_file_read(const char* filepath);

/* Data files are simple json objects with array of entries

wifi.json

{
    "data": [
        {"ssid": "ABCD", "password": "12345678"},
        {"ssid": "EFGH", "password": "87654321"}
    ]
}

ntrip.json

{
    "data": [
        {"server": "0.0.0.0", "port": "2101", "mntpoint": "ABC", "username": "user", "password": "1234"},
        {"server": "0.0.0.0", "port": "2101", "mntpoint": "ABC", "username": "user", "password": "1234"}
    ]
}

base.json

{
    "data": [
        {"lat": "0.000000000", "lon": "0.000000000", "height": "0.0000"},
        {"lat": "0.000000000", "lon": "0.000000000", "height": "0.0000"}
    ]
}
*/

// convert input string to CJson object and append to data object
esp_err_t storage_file_write_entry(const char* filepath, const char* serialized_object);

// delete object at the n-th index in the data object
esp_err_t storage_file_delete_entry(const char* filepath, uint8_t index);

#endif  // STORAGE_H
