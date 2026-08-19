#include "uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "status.h"
#include "ublox.h"
#include "util.h"

static const char* TAG1 = "uart1";
static const char* TAG2 = "uart2";

static QueueHandle_t g_uart1_event_queue = NULL;
static QueueHandle_t g_uart2_event_queue = NULL;

esp_err_t uart_init(void)
{
    // Configure UART1
    uart_config_t uart1_config = {
        .baud_rate = UART_BAUD_RATE_DEFAULT,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART1_PORT, &uart1_config));
    ESP_ERROR_CHECK(uart_set_pin(UART1_PORT, UART1_TX_PIN, UART1_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART1_PORT, UART1_RX_BUFFER_SIZE, 0, UART1_EVENT_QUEUE_SIZE, &g_uart1_event_queue, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG1, "initialized at baud rate " TOSTRING(UART_BAUD_RATE_DEFAULT));

    // At startup, assume the UBlox is in its default baud rate of
    // UART_BAUD_RATE_DEFAULT so, change all UART baud rates to UART_BAUD_RATE_HIGH
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH));
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH));  // UART1 should be changed last
    vTaskDelay(pdMS_TO_TICKS(100));

    // Also change the ESP32 UART1 baud rate to UART_BAUD_RATE_HIGH
    ESP_ERROR_CHECK(uart_set_baudrate(UART1_PORT, UART_BAUD_RATE_HIGH));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG1, "baud rate changed to " TOSTRING(UART_BAUD_RATE_HIGH));

    // Configure UART2
    uart_config_t uart2_config = {
        .baud_rate = UART_BAUD_RATE_HIGH,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART2_PORT, &uart2_config));
    ESP_ERROR_CHECK(uart_set_pin(UART2_PORT, UART2_TX_PIN, UART2_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART2_PORT, UART2_RX_BUFFER_SIZE, 0, UART2_EVENT_QUEUE_SIZE, &g_uart2_event_queue, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG2, "initialized at baud rate " TOSTRING(UART_BAUD_RATE_HIGH));

    return ESP_OK;
}

QueueHandle_t uart1_get_event_queue(void)
{
    return g_uart1_event_queue;
}

QueueHandle_t uart2_get_event_queue(void)
{
    return g_uart2_event_queue;
}

void uart1_send_command(const char* msg)
{
    if (!msg || strlen(msg) == 0)
    {
        return;
    }

    static uint8_t buffer[64];
    uint32_t n = ubx_gen_cmd(msg, buffer);
    uart_write_bytes(UART1_PORT, buffer, n);
    uart_wait_tx_done(UART1_PORT, portMAX_DELAY);
    ESP_LOGI(TAG1, "sent command: %s", msg);
    vTaskDelay(pdMS_TO_TICKS(50));  // wait for UBlox to process the command
}

void uart2_send_data(const uint8_t* data, size_t length)
{
    if (!data || length == 0)
    {
        return;
    }

    uart_write_bytes(UART2_PORT, (const char*)data, length);
    ESP_LOGI(TAG2, "sent %zu bytes", length);
}
