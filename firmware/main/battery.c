#include "battery.h"

#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"

static const char* TAG = "battery";

static adc_oneshot_unit_handle_t g_bat_volt_adc_handle = NULL;
static adc_cali_handle_t g_adc_cali_handle = NULL;

static void battery_task(void* arg)
{
    int adc_reading = 0;

    while (1)
    {
        esp_err_t err = adc_oneshot_read(g_bat_volt_adc_handle, BAT_VOLT_ADC_CHAN, &adc_reading);
        if (err == ESP_OK)
        {
            if (g_adc_cali_handle != NULL)
            {
                int voltage = 0;
                err = adc_cali_raw_to_voltage(g_adc_cali_handle, adc_reading, &voltage);
                if (err == ESP_OK)
                {
                    status_set(STT_BAT_VOLT, voltage);
                }
                else
                {
                    status_set(STT_BAT_VOLT, adc_reading);
                }
            }
            else
            {
                status_set(STT_BAT_VOLT, adc_reading);
            }
        }
        else
        {
            status_set(STT_BAT_VOLT, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t battery_init()
{
    esp_err_t err = ESP_OK;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BAT_VOLT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    err = adc_oneshot_new_unit(&init_config, &g_bat_volt_adc_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init ADC unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,  // in datasheet, effective measurement range of 150 ∼ 2450 mV
    };

    err = adc_oneshot_config_channel(g_bat_volt_adc_handle, BAT_VOLT_ADC_CHAN, &channel_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to config ADC channel: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(g_bat_volt_adc_handle);
        g_bat_volt_adc_handle = NULL;
        return err;
    }

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BAT_VOLT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    err = adc_cali_create_scheme_curve_fitting(&cali_config, &g_adc_cali_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to create calibration scheme: %s", esp_err_to_name(err));
        g_adc_cali_handle = NULL;
    }

    BaseType_t task_created = xTaskCreate(battery_task, "battery", 1024, NULL, 1, NULL);

    if (task_created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create battery task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery task created successfully");
    return ESP_OK;
}
