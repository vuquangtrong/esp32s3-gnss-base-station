#ifndef BATTERY_H
#define BATTERY_H

#include <esp_err.h>

#define BAT_VOLT_ADC_GPIO GPIO_NUM_3  // GPIO3 is connected to ADC channel 2 of ADC1 on ESP32-S3
#define BAT_VOLT_ADC_UNIT ADC_UNIT_1
#define BAT_VOLT_ADC_CHAN ADC_CHANNEL_2

esp_err_t battery_init();

#endif  // BATTERY_H
