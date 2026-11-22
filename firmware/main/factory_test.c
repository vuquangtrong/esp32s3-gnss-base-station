#include "factory_test.h"

#if FACTORY_TEST

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart.h"

static const char* TAG = "factory_test";

static uint8_t g_uart1_test_buf[UART_RX_BUFFER_SIZE];
static uint8_t g_uart2_test_buf[UART_RX_BUFFER_SIZE];

static void factory_test_uart_task(void* arg)
{
    uart_port_t port = (uart_port_t)(uintptr_t)arg;
    uint8_t* buf = NULL;
    uint8_t count = 0;

    if (port == UART1_PORT)
    {
        buf = g_uart1_test_buf;
    }
    else if (port == UART2_PORT)
    {
        buf = g_uart2_test_buf;
    }
    else
    {
        vTaskDelete(NULL);
        return;
    }

    uint32_t current_baudrate = 0;
    esp_err_t err = uart_get_baudrate(port, &current_baudrate);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Current UART%d baud rate: %lu", port, current_baudrate);
    }
    else
    {
        ESP_LOGE(TAG, "UART%d failed to get baud rate (error: %s)", port, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        int len = uart_read_bytes(port, buf, UART_RX_BUFFER_SIZE, pdMS_TO_TICKS(UART_RX_TIMEOUT_MS));
        if (len > 0)
        {
            ESP_LOGI(TAG, "UART%d read %d B\n%.*s", port, len, len, (const char*)buf);
        }

        count++;
        if (count >= 5)
        {
            ESP_LOGI(TAG, "UART%d read task exiting after 5 attempts", port);
            vTaskDelete(NULL);
        }
    }
}

esp_err_t factory_test_uart(void)
{
    // enable NMEA messages for factory test
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART1 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 1");

    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART2 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART2 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART2 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART2 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART2 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART2 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 1");

    // create tasks to read NMEA messages from UART1 and UART2
    if (xTaskCreate(factory_test_uart_task, "factory_test_uart1_task", 4096, (void*)(uintptr_t)UART1_PORT, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "UART%d failed to create read task", UART1_PORT);
        return ESP_FAIL;
    }
    if (xTaskCreate(factory_test_uart_task, "factory_test_uart2_task", 4096, (void*)(uintptr_t)UART2_PORT, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "UART%d failed to create read task", UART2_PORT);
        return ESP_FAIL;
    }

    return ESP_OK;
}

#endif
