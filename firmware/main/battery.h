#ifndef BATTERY_H
#define BATTERY_H

#include <esp_err.h>

esp_err_t battery_init();
esp_err_t battery_task_start(void);

#endif  // BATTERY_H
