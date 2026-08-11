#include "uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logger.h"
#include "parser.h"
#include "status.h"
#include "ublox.h"
#include "util.h"

static const char* TAG1 = "uart1";
static const char* TAG2 = "uart2";

static QueueHandle_t g_uart1_event_queue = NULL;
static QueueHandle_t g_uart2_event_queue = NULL;

static TaskHandle_t g_uart1_task_handle = NULL;
static TaskHandle_t g_uart2_task_handle = NULL;

static void uart1_send_command(const char* msg)
{
    if (msg == NULL || strlen(msg) == 0)
    {
        return;
    }

    static uint8_t buffer[64];
    uint32_t n = ubx_gen_cmd(msg, buffer);
    uart_write_bytes(UART1_NUM, buffer, n);
    uart_wait_tx_done(UART1_NUM, portMAX_DELAY);
    ESP_LOGI(TAG1, "sent command: %s", msg);
}

// UBlox UART1 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol with GGA, GLL, GSA, GSV, RMC, VTG, TXT messages are output
//      by default. UBX and RTCM 3.3 protocols are enabled by default but no
//      output messages are enabled by default
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      UBX, NMEA and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

// UBlox UART2 default configuration:
// TX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA and UBX protocols are disabled by default.
//      RTCM 3.3 protocols is enabled by default but no output messages are
//      enabled by default
// RX:  38400 baud, 8 bits, no parity bit, 1 stop bit.
//      NMEA protocol is disabled by default.
//      UBX and RTCM 3.3 input protocols are enabled by default.
//      SPARTN input protocol is enabled by default.

static void ublox_set_mode_rover()
{
    // In ROVER mode:
    // ESP32 reads GGA and PVT messages from UBlox on UART1 (RX).
    // ESP32 sends RTCM3 correction data to UBlox on UART2 (TX).

    // UBlox UART1 TX
    // enable NMEA protocol and enable GGA message
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GGA_UART1 10");  // once every 10 epochs
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GLL_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSA_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_GSV_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_RMC_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_VTG_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-NMEA_ID_TXT_UART1 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-NMEA 1");
    // disable RTCM3 protocol,
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-RTCM3 0");
    // enable UBX protocol, and enable NAV-PVT message
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-MSGOUT-UBX_NAV_PVT_UART1 1");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1OUTPROT-UBX 1");

    // UBlox UART1 RX
    // disable NMEA protocol,
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-NMEA 0");
    // disable RTCM3 protocol,
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-RTCM3 0");
    // disable SPARTN protocol,
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-SPARTN 0");
    // enable UBX protocol
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1INPROT-UBX 1");

    // UBlox UART2 TX
    // disable NMEA, UBX and RTCM3 protocols
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-NMEA 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-UBX 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2OUTPROT-RTCM3 0");
    // UBlox UART2 RX
    // disable NMEA, UBX and SPARTN protocols, enable RTCM3 protocol
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-NMEA 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-UBX 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-SPARTN 0");
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2INPROT-RTCM3 1");

    status_set(STT_GNSS_MODE, GNSS_ROVER);
}

static uart_buf_t g_uart1_buf_pool[UART_BUF_POOL_SIZE] = {0};
static portMUX_TYPE g_uart1_buf_pool_spinlock = portMUX_INITIALIZER_UNLOCKED;

uart_buf_t* uart1_buf_acquire(void)
{
    uart_buf_t* found = NULL;
    portENTER_CRITICAL(&g_uart1_buf_pool_spinlock);
    for (int i = 0; i < UART_BUF_POOL_SIZE; i++)
    {
        if (g_uart1_buf_pool[i].ref_count == 0)
        {
            g_uart1_buf_pool[i].ref_count = 1;
            g_uart1_buf_pool[i].length = 0;
            found = &g_uart1_buf_pool[i];
            break;
        }
    }
    portEXIT_CRITICAL(&g_uart1_buf_pool_spinlock);

    if (found == NULL)
    {
        ESP_LOGW(TAG1, "buffer pool exhausted");
    }
    return found;
}

void uart1_buf_release(uart_buf_t* buf)
{
    if (buf == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&g_uart1_buf_pool_spinlock);
    if (buf->ref_count > 0)
    {
        buf->ref_count--;
    }
    portEXIT_CRITICAL(&g_uart1_buf_pool_spinlock);
}

static void uart1_task(void* arg)
{
    uart_event_t event;

    ESP_LOGI(TAG1, "task started, waiting for events...");
    uart_flush_input(UART1_NUM);
    while (1)
    {
        if (xQueueReceive(g_uart1_event_queue, (void*)&event, portMAX_DELAY))
        {
            switch (event.type)
            {
                case UART_DATA:
                {
                    size_t buffered_len = 0;
                    uart_get_buffered_data_len(UART1_NUM, &buffered_len);
                    while (buffered_len > 0)
                    {
                        uart_buf_t* buf = uart1_buf_acquire();
                        if (buf == NULL)
                        {
                            break;
                        }
                        int read_size = (buffered_len > UART_BUF_PAYLOAD_SIZE) ? UART_BUF_PAYLOAD_SIZE : (int)buffered_len;
                        int len = uart_read_bytes(UART1_NUM, buf->data, read_size, pdMS_TO_TICKS(100));
                        if (len > 0)
                        {
                            buf->length = (uint16_t)len;
                            int posted_count = 0;

                            if (parser_post_buf(buf) == ESP_OK)
                            {
                                posted_count++;
                            }
                            if (logger_post_buf(buf) == ESP_OK)
                            {
                                posted_count++;
                            }

                            if (posted_count > 0)
                            {
                                portENTER_CRITICAL(&g_uart1_buf_pool_spinlock);
                                buf->ref_count = posted_count;
                                portEXIT_CRITICAL(&g_uart1_buf_pool_spinlock);
                            }
                            else
                            {
                                uart1_buf_release(buf);
                            }
                        }
                        else
                        {
                            uart1_buf_release(buf);
                        }
                        uart_get_buffered_data_len(UART1_NUM, &buffered_len);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG1, "FIFO overflow");
                    uart_flush_input(UART1_NUM);
                    xQueueReset(g_uart1_event_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG1, "buffer full");
                    uart_flush_input(UART1_NUM);
                    xQueueReset(g_uart1_event_queue);
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGE(TAG1, "parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGE(TAG1, "frame error");
                    break;
                default:
                    ESP_LOGW(TAG1, "Unknown event type: %d", event.type);
                    break;
            }
        }
    }
}

static void uart2_task(void* arg)
{
    uart_event_t event;
    uint8_t* uart2_rx_buffer = heap_caps_malloc(UART2_RX_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!uart2_rx_buffer)
    {
        ESP_LOGE(TAG2, "failed to allocate read buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG2, "task started, waiting for events...");
    uart_flush_input(UART2_NUM);
    while (1)
    {
        if (xQueueReceive(g_uart2_event_queue, (void*)&event, portMAX_DELAY))
        {
            switch (event.type)
            {
                case UART_DATA:
                    ESP_LOGI(TAG2, "data received: %d bytes", event.size);
                    int len = uart_read_bytes(UART2_NUM, uart2_rx_buffer, event.size, pdMS_TO_TICKS(100));
                    if (len > 0)
                    {
                        // not handling this data now
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG2, "FIFO overflow");
                    uart_flush_input(UART2_NUM);
                    xQueueReset(g_uart2_event_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG2, "buffer full");
                    uart_flush_input(UART2_NUM);
                    xQueueReset(g_uart2_event_queue);
                    break;
                case UART_PARITY_ERR:
                    ESP_LOGE(TAG2, "parity error");
                    break;
                case UART_FRAME_ERR:
                    ESP_LOGE(TAG2, "frame error");
                    break;
                default:
                    ESP_LOGW(TAG2, "Unknown event type: %d", event.type);
                    break;
            }
        }
    }
}

esp_err_t uart_init(void)
{
    // Configure UART1
    uart_config_t uart1_config = {
        .baud_rate = UART_BAUD_RATE_DEF,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART1_NUM, &uart1_config));
    ESP_ERROR_CHECK(uart_set_pin(UART1_NUM, UART1_TX_PIN, UART1_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART1_NUM, UART1_RX_BUFFER_SIZE, 0, UART1_EVENT_QUEUE_SIZE, &g_uart1_event_queue, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG1, "initialized at baud rate " TOSTRING(UART_BAUD_RATE_DEF));

    // At startup, assume the UBlox is in its default baud rate of
    // UART_BAUD_RATE_DEF so, change all UART baud rates to UART_BAUD_RATE_HIGH
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART2-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH));
    uart1_send_command("CFG-VALSET 0 1 0 0 CFG-UART1-BAUDRATE " TOSTRING(UART_BAUD_RATE_HIGH));  // UART1 should be changed last
    vTaskDelay(pdMS_TO_TICKS(100));

    // Also change the ESP32 UART1 baud rate to UART_BAUD_RATE_HIGH
    ESP_ERROR_CHECK(uart_set_baudrate(UART1_NUM, UART_BAUD_RATE_HIGH));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG1, "baud rate changed to " TOSTRING(UART_BAUD_RATE_HIGH));

    // Set UBlox to ROVER mode
    ublox_set_mode_rover();

    // Configure UART2
    uart_config_t uart2_config = {
        .baud_rate = UART_BAUD_RATE_HIGH,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(UART2_NUM, &uart2_config));
    ESP_ERROR_CHECK(uart_set_pin(UART2_NUM, UART2_TX_PIN, UART2_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART2_NUM, UART2_RX_BUFFER_SIZE, 0, UART2_EVENT_QUEUE_SIZE, &g_uart2_event_queue, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG2, "initialized at baud rate " TOSTRING(UART_BAUD_RATE_HIGH));

    return ESP_OK;
}

esp_err_t uart1_task_start(void)
{
    if (!g_uart1_event_queue)
    {
        ESP_LOGE(TAG1, "UART1 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(uart1_task, "uart1_rx", 8192, NULL, 6, &g_uart1_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG1, "failed to create UART task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t uart2_task_start(void)
{
    if (!g_uart2_event_queue)
    {
        ESP_LOGE(TAG2, "UART2 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ret = xTaskCreate(uart2_task, "uart2_rx", 8192, NULL, 6, &g_uart2_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG2, "failed to create UART task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void uart2_send_data(const uint8_t* data, size_t length)
{
    if (!data || length == 0)
    {
        return;
    }

    uart_write_bytes(UART2_NUM, (const char*)data, length);
    ESP_LOGI(TAG2, "sent %zu bytes", length);
}
