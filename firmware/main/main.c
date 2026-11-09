#include <stdio.h>
#include <string.h>

#include "battery.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/uart.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "factory_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"
#include "uart.h"
#include "ublox.h"
#include "util.h"

static const char* TAG = "factory";

static adc_oneshot_unit_handle_t g_bat_adc_handle = NULL;
static adc_cali_handle_t g_bat_cali_handle = NULL;

static sdmmc_card_t* g_sdcard = NULL;

static ft_uart_monitor_t g_uart1_monitor = {.port = UART1_PORT, .tag = "UART1", .line_length = 0, .line = {0}};
static ft_uart_monitor_t g_uart2_monitor = {.port = UART2_PORT, .tag = "UART2", .line_length = 0, .line = {0}};

/* LED -----------------------------------------------------------------------*/

static void ft_led_task(void* arg)
{
    uint32_t level = 0;

    while (1)
    {
        level ^= 1;
        gpio_set_level(FT_LED_GPIO, level);
        vTaskDelay(pdMS_TO_TICKS(FT_LED_BLINK_PERIOD_MS));
    }
}

static esp_err_t ft_led_init(void)
{
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << FT_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&led_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LED: failed to config GPIO%d: %s", FT_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(ft_led_task, "led", FT_LED_TASK_STACK_SIZE, NULL, FT_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "LED: failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LED: blinking on GPIO%d", FT_LED_GPIO);
    return ESP_OK;
}

/* UART ----------------------------------------------------------------------*/

static void ft_ublox_send_command(const char* command)
{
    if (command == NULL || command[0] == '\0')
    {
        return;
    }

    static uint8_t frame[FT_UBX_FRAME_LENGTH_MAX];
    uint32_t frame_length = ubx_gen_cmd(command, frame);
    if (frame_length == 0)
    {
        ESP_LOGE(TAG, "UBlox: failed to generate command: %s", command);
        return;
    }

    uart_write_bytes(UART1_PORT, frame, frame_length);
    uart_wait_tx_done(UART1_PORT, portMAX_DELAY);
    ESP_LOGI(TAG, "UBlox: sent command: %s", command);
    vTaskDelay(pdMS_TO_TICKS(50));  // wait for UBlox to process the command
}

static esp_err_t ft_uart_install(uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin, int32_t baud_rate)
{
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t err = uart_param_config(port, &uart_config);
    if (err == ESP_OK)
    {
        err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK)
    {
        err = uart_driver_install(port, FT_UART_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART%d: failed to install: %s", port, esp_err_to_name(err));
    }

    return err;
}

static void ft_uart_print_line(ft_uart_monitor_t* monitor)
{
    if (monitor->line_length == 0)
    {
        return;
    }

    monitor->line[monitor->line_length] = '\0';
    printf("[%s] %s\n", monitor->tag, monitor->line);
    monitor->line_length = 0;
}

static void ft_uart_task(void* arg)
{
    ft_uart_monitor_t* monitor = (ft_uart_monitor_t*)arg;
    uint8_t chunk[FT_UART_READ_CHUNK_SIZE] = {0};

    while (1)
    {
        int32_t read_length = uart_read_bytes(monitor->port, chunk, sizeof(chunk), pdMS_TO_TICKS(FT_UART_READ_TIMEOUT_MS));

        for (int32_t i = 0; i < read_length; i++)
        {
            uint8_t byte = chunk[i];

            if (byte == '\n')
            {
                ft_uart_print_line(monitor);
            }
            else if (byte >= 0x20 && byte <= 0x7E)
            {
                // Keep printable characters only, binary UBX/RTCM bytes are dropped
                monitor->line[monitor->line_length++] = (char)byte;
                if (monitor->line_length >= (FT_UART_LINE_LENGTH_MAX - 1))
                {
                    ft_uart_print_line(monitor);
                }
            }
        }
    }
}

static esp_err_t ft_uart_init(void)
{
    // In case UBlox UARTs are still at the high baud rate set by the main firmware,
    // revert them to the default baud rate. UART1 should be changed last
    esp_err_t err = ft_uart_install(UART1_PORT, UART1_TX_PIN, UART1_RX_PIN, UART_BAUD_RATE_HIGH);
    if (err != ESP_OK)
    {
        return err;
    }
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-UART2-BAUDRATE " TOSTRING(UART_BAUD_RATE_DEFAULT));
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-UART1-BAUDRATE " TOSTRING(UART_BAUD_RATE_DEFAULT));
    vTaskDelay(pdMS_TO_TICKS(100));

    err = uart_set_baudrate(UART1_PORT, UART_BAUD_RATE_DEFAULT);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART1: failed to set baud rate: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "UART1: initialized at baud rate " TOSTRING(UART_BAUD_RATE_DEFAULT));

    err = ft_uart_install(UART2_PORT, UART2_TX_PIN, UART2_RX_PIN, UART_BAUD_RATE_DEFAULT);
    if (err != ESP_OK)
    {
        return err;
    }
    ESP_LOGI(TAG, "UART2: initialized at baud rate " TOSTRING(UART_BAUD_RATE_DEFAULT));

    // UBlox UART1 outputs NMEA messages by default.
    // UBlox UART2 has NMEA output disabled by default, so enable it with GGA and RMC messages
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-UART2-ENABLED 1");
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART2 1");
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART2 1");
    ft_ublox_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 1");

    uart_flush_input(UART1_PORT);
    uart_flush_input(UART2_PORT);

    if (xTaskCreate(ft_uart_task, "uart1", FT_UART_TASK_STACK_SIZE, &g_uart1_monitor, FT_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(ft_uart_task, "uart2", FT_UART_TASK_STACK_SIZE, &g_uart2_monitor, FT_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "UART: failed to create monitor tasks");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Battery -------------------------------------------------------------------*/

static void ft_battery_task(void* arg)
{
    while (1)
    {
        int32_t raw_sum = 0;
        uint32_t sample_count = 0;

        for (uint32_t i = 0; i < FT_BAT_SAMPLE_COUNT; i++)
        {
            int raw = 0;
            if (adc_oneshot_read(g_bat_adc_handle, BAT_VOLT_ADC_CHAN, &raw) == ESP_OK)
            {
                raw_sum += raw;
                sample_count++;
            }
        }

        if (sample_count == 0)
        {
            ESP_LOGE(TAG, "Battery: failed to read ADC");
        }
        else
        {
            int raw_average = (int)(raw_sum / (int32_t)sample_count);
            int voltage_mv = 0;

            if (g_bat_cali_handle != NULL && adc_cali_raw_to_voltage(g_bat_cali_handle, raw_average, &voltage_mv) == ESP_OK)
            {
                ESP_LOGI(TAG, "Battery: raw=%d, voltage=%d mV", raw_average, voltage_mv);
            }
            else
            {
                ESP_LOGI(TAG, "Battery: raw=%d, voltage=N/A (no calibration)", raw_average);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FT_BAT_REPORT_PERIOD_MS));
    }
}

static esp_err_t ft_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = BAT_VOLT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err = adc_oneshot_new_unit(&unit_config, &g_bat_adc_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Battery: failed to init ADC unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };

    err = adc_oneshot_config_channel(g_bat_adc_handle, BAT_VOLT_ADC_CHAN, &channel_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Battery: failed to config ADC channel: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(g_bat_adc_handle);
        g_bat_adc_handle = NULL;
        return err;
    }

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BAT_VOLT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    err = adc_cali_create_scheme_curve_fitting(&cali_config, &g_bat_cali_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Battery: no ADC calibration: %s", esp_err_to_name(err));
        g_bat_cali_handle = NULL;
    }

    if (xTaskCreate(ft_battery_task, "battery", FT_BATTERY_TASK_STACK_SIZE, NULL, FT_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Battery: failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery: reading ADC on GPIO%d", BAT_VOLT_ADC_GPIO);
    return ESP_OK;
}

/* SD card -------------------------------------------------------------------*/

static esp_err_t ft_sdcard_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = FT_SDCARD_MAX_FILES,
        .allocation_unit_size = SDCARD_ALLOCATION_UNIT_SIZE,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDCARD_FREQ_KHZ;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = SDCARD_BUS_WIDTH;
    slot_config.clk = SDCARD_PIN_CLK;
    slot_config.cmd = SDCARD_PIN_CMD;
    slot_config.d0 = SDCARD_PIN_D0;
    slot_config.d1 = SDCARD_PIN_D1;
    slot_config.d2 = SDCARD_PIN_D2;
    slot_config.d3 = SDCARD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &g_sdcard);
    if (err != ESP_OK)
    {
        g_sdcard = NULL;
        ESP_LOGE(TAG, "SD card: failed to mount: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD card: mounted at %s", SDCARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, g_sdcard);

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK)
    {
        ESP_LOGI(TAG, "SD card: FAT total=%llu MB, free=%llu MB", total_bytes / (1024 * 1024), free_bytes / (1024 * 1024));
    }

    return ESP_OK;
}

static void ft_sdcard_unmount(void)
{
    if (g_sdcard == NULL)
    {
        return;
    }

    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, g_sdcard);
    g_sdcard = NULL;
    ESP_LOGW(TAG, "SD card: removed");
}

static void ft_sdcard_task(void* arg)
{
    while (1)
    {
        if (g_sdcard == NULL)
        {
            ft_sdcard_mount();
        }
        else if (sdmmc_get_status(g_sdcard) != ESP_OK)
        {
            ft_sdcard_unmount();
        }

        vTaskDelay(pdMS_TO_TICKS(FT_SDCARD_CHECK_PERIOD_MS));
    }
}

static esp_err_t ft_sdcard_init(void)
{
    // The task mounts the card and prints its info, then keeps checking for removal/insertion
    if (xTaskCreate(ft_sdcard_task, "sdcard", FT_SDCARD_TASK_STACK_SIZE, NULL, FT_TASK_PRIORITY, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "SD card: failed to create task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Main ----------------------------------------------------------------------*/

void app_main(void)
{
    printf("\nFACTORY TEST V" VERSION " " GIT_COMMIT " " BUILD_TIME "\n");

    esp_err_t led_err = ft_led_init();
    esp_err_t battery_err = ft_battery_init();
    esp_err_t sdcard_err = ft_sdcard_init();
    esp_err_t uart_err = ft_uart_init();

    ESP_LOGI(TAG, "Init LED: %s, Battery: %s, SD card: %s, UART: %s",
             esp_err_to_name(led_err), esp_err_to_name(battery_err), esp_err_to_name(sdcard_err), esp_err_to_name(uart_err));
}
