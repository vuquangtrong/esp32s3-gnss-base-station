#ifndef UART_H
#define UART_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "status.h"

#define UART_BAUD_RATE_DEFAULT 38400
#define UART_BAUD_RATE_HIGH    921600  // maybe lower at 460800, 230400

// UART1 on ESP32-S3 connects to UART1 on UBlox
#define UART1_PORT             UART_NUM_1
#define UART1_TX_PIN           GPIO_NUM_40
#define UART1_RX_PIN           GPIO_NUM_41
#define UART1_RX_BUFFER_SIZE   2048
#define UART1_EVENT_QUEUE_SIZE 10

// UART2 on ESP32-S3 connects to UART2 on UBlox
#define UART2_PORT             UART_NUM_2
#define UART2_TX_PIN           GPIO_NUM_38
#define UART2_RX_PIN           GPIO_NUM_39
#define UART2_RX_BUFFER_SIZE   2048
#define UART2_EVENT_QUEUE_SIZE 10

esp_err_t uart_init(void);

QueueHandle_t uart1_get_event_queue(void);
QueueHandle_t uart2_get_event_queue(void);

void uart1_send_command(const char* command);
void uart2_send_data(const uint8_t* data, size_t length);

#endif  // UART_H
