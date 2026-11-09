#ifndef FACTORY_TEST_H
#define FACTORY_TEST_H

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"

// LED
#define FT_LED_GPIO            GPIO_NUM_4
#define FT_LED_BLINK_PERIOD_MS 500

// UART
// Pins and baud rates are shared with the main firmware, see uart.h
#define FT_UART_RX_BUFFER_SIZE    2048
#define FT_UART_READ_CHUNK_SIZE   256
#define FT_UART_READ_TIMEOUT_MS   100
#define FT_UART_LINE_LENGTH_MAX   128
#define FT_UBX_FRAME_LENGTH_MAX   64

// Battery
#define FT_BAT_SAMPLE_COUNT      8
#define FT_BAT_REPORT_PERIOD_MS  2000

// SD card
#define FT_SDCARD_CHECK_PERIOD_MS 3000
#define FT_SDCARD_MAX_FILES       4

// Tasks
#define FT_LED_TASK_STACK_SIZE     2048
#define FT_UART_TASK_STACK_SIZE    4096
#define FT_BATTERY_TASK_STACK_SIZE 3072
#define FT_SDCARD_TASK_STACK_SIZE  4096
#define FT_TASK_PRIORITY           1

typedef struct
{
    uart_port_t port;
    const char* tag;
    uint32_t line_length;
    char line[FT_UART_LINE_LENGTH_MAX];
} ft_uart_monitor_t;

#endif  // FACTORY_TEST_H
