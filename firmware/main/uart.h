#ifndef UART_H
#define UART_H

#include "driver/gpio.h"
#include "esp_err.h"

#define UART_BAUD_RATE_DEF  38400
#define UART_BAUD_RATE_HIGH 921600  // 460800

// UART1 on ESP32-S3 connects to UART1 on UBlox
#define UART1_NUM              UART_NUM_1
#define UART1_TX_PIN           GPIO_NUM_40
#define UART1_RX_PIN           GPIO_NUM_41
#define UART1_RX_BUFFER_SIZE   2048
#define UART1_EVENT_QUEUE_SIZE 10

// UART2 on ESP32-S3 connects to UART2 on UBlox
#define UART2_NUM              UART_NUM_2
#define UART2_TX_PIN           GPIO_NUM_38
#define UART2_RX_PIN           GPIO_NUM_39
#define UART2_RX_BUFFER_SIZE   2048
#define UART2_EVENT_QUEUE_SIZE 10

// UART buffer pool for receiving data from UBlox
#define UART_BUF_PAYLOAD_SIZE 1024
#define UART_BUF_POOL_SIZE    32

typedef struct
{
    uint8_t data[UART_BUF_PAYLOAD_SIZE];
    uint16_t length;
    volatile int ref_count;
} uart_buf_t;

esp_err_t uart_init(void);
esp_err_t uart1_task_start(void);
esp_err_t uart2_task_start(void);

uart_buf_t* uart1_buf_acquire(void);
void uart1_buf_release(uart_buf_t* buf);

#endif  // UART_H
