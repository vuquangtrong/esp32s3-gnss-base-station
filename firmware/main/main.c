#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "driver/sdspi_host.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_vfs_fat.h"
#include "esp_private/sdmmc_common.h"
#include "esp_log.h"
#include "ublox.h"

static const char* TAG = "factort_test";

/// LED  ////////////////////////////////

#define LED_GPIO GPIO_NUM_4

static void configure_led(void)
{
    gpio_reset_pin(LED_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

static void led_task(void* arg)
{
    configure_led();

    uint8_t led_state = 0;
    while (true)
    {
        /* Toggle the LED state */
        led_state = !led_state;
        gpio_set_level(LED_GPIO, led_state);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

/// TEMPERATURE SENSOR  ////////////////////////////////

temperature_sensor_handle_t temp_sensor = NULL;

static void configure_temp_sensor(void)
{
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
}

static void temp_sensor_task(void* arg)
{
    configure_temp_sensor();

    float temperature;
    ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &temperature));
    printf("Temperature: %.02f °C\n", temperature);

    while (true)
    {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

/// BATTERY  ////////////////////////////////

#define BAT_VOL_ADC_GPIO GPIO_NUM_3  // connected to ADC1_CH2 via IO MUX with Priority 1
#define BAT_VOL_ADC_UNIT ADC_UNIT_1
#define BAT_VOL_ADC_CHAN ADC_CHANNEL_2

static adc_oneshot_unit_handle_t bat_vol_adc_handle = NULL;

static void configure_bat_vol_adc(void)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id  = BAT_VOL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_config, &bat_vol_adc_handle));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(bat_vol_adc_handle, BAT_VOL_ADC_CHAN, &chan_config));
}

static void bat_vol_adc_task(void* arg)
{
    configure_bat_vol_adc();

    int raw;
    ESP_ERROR_CHECK(adc_oneshot_read(bat_vol_adc_handle, BAT_VOL_ADC_CHAN, &raw));
    printf("Battery voltage raw: %d\n", raw);

    while (true)
    {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

/// SD CARD  ////////////////////////////////

#define SDCARD_MOUNT_POINT  "/sdcard"
#define SDCARD_PIN_NUM_MISO GPIO_NUM_6
#define SDCARD_PIN_NUM_MOSI GPIO_NUM_15
#define SDCARD_PIN_NUM_CLK  GPIO_NUM_7
#define SDCARD_PIN_NUM_CS   GPIO_NUM_16

static sdmmc_card_t* card = NULL;

static void configure_sdcard()
{
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {.format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};

    const char mount_point[] = SDCARD_MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = SDCARD_PIN_NUM_MOSI,
        .miso_io_num     = SDCARD_PIN_NUM_MISO,
        .sclk_io_num     = SDCARD_PIN_NUM_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = SDCARD_PIN_NUM_CS;
    slot_config.host_id               = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(
                TAG,
                "Failed to mount filesystem. "
                "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option."
            );
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Failed to initialize the card (%s). "
                "Make sure SD card lines have pull-up resistors in place.",
                esp_err_to_name(ret)
            );
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    sdmmc_card_print_info(stdout, card);
}

static void sdcard_task(void* arg)
{
    configure_sdcard();

    while (true)
    {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

/// UART ////////////////////////////////

#define UART1_TX_PIN GPIO_NUM_40
#define UART1_RX_PIN GPIO_NUM_41
#define UART2_TX_PIN GPIO_NUM_38
#define UART2_RX_PIN GPIO_NUM_39

#define UBX_MSG_LEN 128

static const int RX_BUF_SIZE = 1024;

void configure_uart(void)
{
    const uart_port_t uart_num1 = UART_NUM_1;
    uart_config_t uart_config1  = {
         .baud_rate  = 38400,
         .data_bits  = UART_DATA_8_BITS,
         .parity     = UART_PARITY_DISABLE,
         .stop_bits  = UART_STOP_BITS_1,
         .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
         .source_clk = UART_SCLK_DEFAULT
    };
    // Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(uart_num1, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(uart_num1, &uart_config1));
    // Set UART pins
    ESP_ERROR_CHECK(uart_set_pin(uart_num1, UART1_TX_PIN, UART1_RX_PIN, GPIO_NUM_NC, GPIO_NUM_NC));

    const uart_port_t uart_num2 = UART_NUM_2;
    uart_config_t uart_config2  = {
         .baud_rate  = 38400,
         .data_bits  = UART_DATA_8_BITS,
         .parity     = UART_PARITY_DISABLE,
         .stop_bits  = UART_STOP_BITS_1,
         .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
         .source_clk = UART_SCLK_DEFAULT
    };
    // Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(uart_num2, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(uart_num2, &uart_config2));
    // Set UART pins
    ESP_ERROR_CHECK(uart_set_pin(uart_num2, UART2_TX_PIN, UART2_RX_PIN, GPIO_NUM_NC, GPIO_NUM_NC));
}

void configure_ublox_uart1()
{
    uint8_t* buffer = calloc(UBX_MSG_LEN, sizeof(uint8_t));
    uint32_t n;

    // UART1, UBX and NEMA is on by default, enable GGA only
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART1 1", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GST_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART1 0", buffer);
    uart_write_bytes(UART_NUM_1, buffer, n);

    free(buffer);
}

void configure_ublox_uart2()
{
    uint8_t* buffer = calloc(UBX_MSG_LEN, sizeof(uint8_t));
    uint32_t n;

    // UART2, NEMA is off by default, so enable NMEA then enable GLL only
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 1", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GST_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART2 1", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);
    n = ubx_gen_cmd("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART2 0", buffer);
    uart_write_bytes(UART_NUM_2, buffer, n);

    free(buffer);
}

void uart_task(void* arg)
{
    configure_uart();
    configure_ublox_uart1();
    configure_ublox_uart2();

    // buffers for reading data
    uint8_t* data1 = (uint8_t*)malloc(RX_BUF_SIZE + 1);
    uint8_t* data2 = (uint8_t*)malloc(RX_BUF_SIZE + 1);

    while (true)
    {
        const int rxBytes1 = uart_read_bytes(UART_NUM_1, data1, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
        data1[rxBytes1]    = 0;
        printf("UART1: Read %d bytes: '%s'\n", rxBytes1, data1);

        const int rxBytes2 = uart_read_bytes(UART_NUM_2, data2, RX_BUF_SIZE, 1000 / portTICK_PERIOD_MS);
        if (rxBytes2 == 0)
        {
            printf("No data from UART2, re-sent config commands.\n");
            configure_ublox_uart2();
            continue;
        }
        data2[rxBytes2] = 0;
        printf("UART2: Read %d bytes: '%s'\n", rxBytes2, data2);
    }

    free(data1);
    free(data2);
}

/// main app entry point  ////////////////////////////////

void app_main(void)
{
    printf("Hello world!\n");

    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(bat_vol_adc_task, "bat_vol_adc_task", 4096, NULL, 5, NULL);
    xTaskCreate(temp_sensor_task, "temp_sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(sdcard_task, "sdcard_task", 8192, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart_task", 8192, NULL, 5, NULL);
}
