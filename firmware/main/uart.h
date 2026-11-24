#ifndef UART_H
#define UART_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define UART_BAUD_RATE_DEFAULT 38400
#define UART_BAUD_RATE_HIGH    460800  // use 921600, 460800, 230400
#define UART_RX_BUFFER_SIZE    4096
#define UART_TX_BUFFER_SIZE    4096
#define UART_RX_TIMEOUT_MS     1000
#define UART_TX_TIMEOUT_MS     1000
#define UART_EVENT_QUEUE_SIZE  20

// UART1 on ESP32-S3 connects to UART1 on UBlox
#define UART1_PORT   UART_NUM_1
#define UART1_TX_PIN GPIO_NUM_40
#define UART1_RX_PIN GPIO_NUM_41

// UART2 on ESP32-S3 connects to UART2 on UBlox
#define UART2_PORT   UART_NUM_2
#define UART2_TX_PIN GPIO_NUM_38
#define UART2_RX_PIN GPIO_NUM_39

esp_err_t uart_init(void);

QueueHandle_t uart1_get_event_queue(void);
QueueHandle_t uart2_get_event_queue(void);

esp_err_t uart1_send_command(const char* command);

#endif  // UART_H
