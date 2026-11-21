#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define SDCARD_MOUNT_POINT          "/sdcard"
#define SDCARD_FREQ_KHZ             SDMMC_FREQ_HIGHSPEED
#define SDCARD_MAX_FILES            8
#define SDCARD_ALLOCATION_UNIT_SIZE (64 * 1024)  // 64KB
#define SDCARD_SYNC_INTERVAL_MS     10000        // 10s
#define SDCARD_BUS_WIDTH            4
#define SDCARD_PIN_CMD              GPIO_NUM_15
#define SDCARD_PIN_CLK              GPIO_NUM_7
#define SDCARD_PIN_D0               GPIO_NUM_6
#define SDCARD_PIN_D1               GPIO_NUM_5
#define SDCARD_PIN_D2               GPIO_NUM_17
#define SDCARD_PIN_D3               GPIO_NUM_16

esp_err_t sdcard_init(void);
esp_err_t sdcard_remount(void);

FILE* sdcard_file_open(const char* filename, const char* mode);
size_t sdcard_file_write(FILE* file, const void* data, size_t size);
esp_err_t sdcard_file_close(FILE* file);

const char* sdcard_list_ubx_files(void);

#endif  // SDCARD_H
