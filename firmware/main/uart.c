#include "uart.h"

#include <stdint.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ublox.h"
#include "util.h"

static const char* TAG1 = "uart1";
static const char* TAG2 = "uart2";

esp_err_t uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE_DEFAULT,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // Configure UART1
    ESP_ERROR_CHECK(uart_param_config(UART1_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART1_PORT, UART1_TX_PIN, UART1_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART1_PORT, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 0, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG1, "initialized at baud rate %d", uart_config.baud_rate);

    // At startup, assume the UBlox is set at UART_BAUD_RATE_DEFAULT
    // so, change all UART baud rates to UART_BAUD_RATE_HIGH
    // UART1 should be changed at the end to avoid losing the connection
    ESP_RETURN_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH)), TAG1, "Can not set baudrate");
    ESP_RETURN_ON_ERROR(uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH)), TAG1, "Can not set baudrate");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG1, "sent command to change UBlox UART baud rates to " TOSTRING(UART_BAUD_RATE_HIGH));

    // Also change the ESP32 UART1 baud rate to UART_BAUD_RATE_HIGH
    ESP_ERROR_CHECK(uart_set_baudrate(UART1_PORT, UART_BAUD_RATE_HIGH));
    vTaskDelay(pdMS_TO_TICKS(500));
    // do not flush the UART1 RX buffer here, as it may contain responses from the UBlox to the baud rate change commands
    ESP_LOGI(TAG1, "baud rate changed to " TOSTRING(UART_BAUD_RATE_HIGH));

    // Configure UART2
    uart_config.baud_rate = UART_BAUD_RATE_HIGH;
    ESP_ERROR_CHECK(uart_param_config(UART2_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART2_PORT, UART2_TX_PIN, UART2_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART2_PORT, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 0, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG2, "initialized at baud rate %d", uart_config.baud_rate);

    return ESP_OK;
}

esp_err_t uart1_send_command(const char* msg)
{
    if (!msg || msg[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    static uint8_t buffer[128];
    uint32_t n = ubx_gen_cmd(msg, buffer, sizeof(buffer));
    if (n == 0)
    {
        ESP_LOGE(TAG1, "failed to generate command: %s", msg);
        return ESP_ERR_INVALID_ARG;
    }

    int written = uart_write_bytes(UART1_PORT, buffer, n);
    if (written < 0 || (uint32_t)written != n)
    {
        ESP_LOGE(TAG1, "failed to write command: %s", msg);
        return ESP_FAIL;
    }

    esp_err_t err = uart_wait_tx_done(UART1_PORT, pdMS_TO_TICKS(UART_TX_TIMEOUT_MS));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG1, "tx done timeout for command: %s", msg);
        return err;
    }

    ESP_LOGI(TAG1, "sent command: %s", msg);
    vTaskDelay(pdMS_TO_TICKS(50));  // wait for UBlox to process the command
    return ESP_OK;
}
