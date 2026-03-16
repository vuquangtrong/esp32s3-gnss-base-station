#include "battery.h"

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "config.h"
#include "status.h"
#include "util.h"

static const char* TAG = "BATT";

#define BAT_VOL_ADC_GPIO GPIO_NUM_3  // connected to ADC1_CH2 via IO MUX with Priority 1
#define BAT_VOL_ADC_UNIT ADC_UNIT_1
#define BAT_VOL_ADC_CHAN ADC_CHANNEL_2

static adc_oneshot_unit_handle_t bat_vol_adc_handle = NULL;

static void battery_task(void* arg)
{
    char buffer[4] = {0};
    int raw;
    int percent;
    while (true)
    {
        ESP_ERROR_CHECK(adc_oneshot_read(bat_vol_adc_handle, BAT_VOL_ADC_CHAN, &raw));
        // 2460 is 100% battery, 1200 is 0% battery, linear interpolation in between
        percent = (raw - 1200) * 100 / (2460 - 1200);
        if (percent < 0)
        {
            percent = 0;
        }
        else if (percent > 100)
        {
            percent = 100;
        }
        snprintf(buffer, sizeof(buffer), "%d", (int)percent);
        status_set(STATUS_BATTERY, buffer);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t battery_init()
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BAT_VOL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &bat_vol_adc_handle));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(bat_vol_adc_handle, BAT_VOL_ADC_CHAN, &chan_config));

    vTaskDelay(pdMS_TO_TICKS(1000));

    xTaskCreate(battery_task, "battery_task", 2048, NULL, 10, NULL);
    return ESP_OK;
}
