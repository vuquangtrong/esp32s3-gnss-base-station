#include "battery.h"

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
    int raw = 0;

    while (1)
    {
        int voltage = 0;
        if (adc_oneshot_read(g_bat_volt_adc_handle, BAT_VOLT_ADC_CHAN, &raw) == ESP_OK && g_adc_cali_handle != NULL)
        {
            if (adc_cali_raw_to_voltage(g_adc_cali_handle, raw, &voltage) != ESP_OK)
            {
                voltage = 0;
            }
        }

        status_set_int(STT_BAT_VOLT, voltage);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BAT_VOLT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err = adc_oneshot_new_unit(&init_config, &g_bat_volt_adc_handle);
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
        .chan = BAT_VOLT_ADC_CHAN,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    err = adc_cali_create_scheme_curve_fitting(&cali_config, &g_adc_cali_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to create calibration scheme: %s", esp_err_to_name(err));
        g_adc_cali_handle = NULL;
    }

    if (xTaskCreate(battery_task, "battery", 2048, NULL, 1, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create battery task");
        if (g_adc_cali_handle != NULL)
        {
            adc_cali_delete_scheme_curve_fitting(g_adc_cali_handle);
            g_adc_cali_handle = NULL;
        }
        adc_oneshot_del_unit(g_bat_volt_adc_handle);
        g_bat_volt_adc_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery task created successfully");
    return ESP_OK;
}
