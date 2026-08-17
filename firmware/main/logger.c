#include "logger.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "logger";

static TaskHandle_t g_logger_task_handle = NULL;
static volatile bool g_logger_running = false;
static FILE* g_log_file = NULL;

// DMA-capable write buffer for efficient SD card writes
static uint8_t* g_write_buffer = NULL;
static size_t g_write_buffer_pos = 0;

static void logger_flush(void)
{
    if (g_log_file == NULL || g_write_buffer_pos == 0)
    {
        return;
    }

    // Check SD card status before writing
    int32_t sdcard_status = status_get_int(STT_SDCARD_STATUS);
    if (sdcard_status == SDCARD_REMOVED || sdcard_status == SDCARD_ERROR)
    {
        ESP_LOGW(TAG, "SD card removed or error detected, stopping logger");
        g_logger_running = false;
        status_set(STT_LOGGER_STATUS, LOGGER_ERROR);
        g_write_buffer_pos = 0;
        return;
    }

    size_t written = sdcard_write(g_log_file, g_write_buffer, g_write_buffer_pos);
    status_set(STT_LOGGER_SIZE, status_get_int(STT_LOGGER_SIZE) + (int)written);
    if (written != g_write_buffer_pos)
    {
        ESP_LOGW(TAG, "partial write: %zu / %zu bytes", written, g_write_buffer_pos);
    }
    g_write_buffer_pos = 0;
}

static void logger_buffer_data(const uint8_t* data, size_t length)
{
    if (data == NULL || length == 0)
    {
        return;
    }

    const uint8_t* src = data;
    size_t remaining = length;

    while (remaining > 0)
    {
        size_t space = LOGGER_WRITE_BUFFER_SIZE - g_write_buffer_pos;
        size_t chunk = (remaining < space) ? remaining : space;

        memcpy(&g_write_buffer[g_write_buffer_pos], src, chunk);
        g_write_buffer_pos += chunk;
        src += chunk;
        remaining -= chunk;

        // Buffer full — flush to SD card
        if (g_write_buffer_pos >= LOGGER_WRITE_BUFFER_SIZE)
        {
            logger_flush();
        }
    }
}

static void logger_task(void* arg)
{
    (void)arg;

    QueueHandle_t uart2_queue = uart2_get_event_queue();
    if (uart2_queue == NULL)
    {
        ESP_LOGE(TAG, "UART2 event queue is NULL");
        g_logger_running = false;
        vTaskDelete(NULL);
        return;
    }

    uart_event_t event = {0};
    static uint8_t rx_buf[512];
    TickType_t last_flush_tick = xTaskGetTickCount();

    while (g_logger_running)
    {
        if (xQueueReceive(uart2_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            switch (event.type)
            {
                case UART_DATA:
                {
                    size_t buffered_len = 0;
                    uart_get_buffered_data_len(UART2_PORT, &buffered_len);
                    while (buffered_len > 0)
                    {
                        size_t read_len = (buffered_len > sizeof(rx_buf)) ? sizeof(rx_buf) : buffered_len;
                        int32_t len = uart_read_bytes(UART2_PORT, rx_buf, read_len, pdMS_TO_TICKS(100));
                        if (len > 0)
                        {
                            logger_buffer_data(rx_buf, (size_t)len);
                        }
                        else
                        {
                            break;
                        }
                        uart_get_buffered_data_len(UART2_PORT, &buffered_len);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART2 hw fifo overflow");
                    uart_flush_input(UART2_PORT);
                    xQueueReset(uart2_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART2 ring buffer full");
                    uart_flush_input(UART2_PORT);
                    xQueueReset(uart2_queue);
                    break;
                default:
                    break;
            }
        }

        // Periodic flush to avoid stale data sitting in the write buffer
        TickType_t now = xTaskGetTickCount();
        if ((now - last_flush_tick) >= pdMS_TO_TICKS(LOGGER_FLUSH_INTERVAL_MS))
        {
            logger_flush();
            last_flush_tick = now;
        }
    }

    // Final flush before exiting
    logger_flush();

    if (g_log_file != NULL)
    {
        sdcard_close(g_log_file);
        g_log_file = NULL;
        ESP_LOGI(TAG, "log file closed");
    }

    if (g_write_buffer != NULL)
    {
        heap_caps_free(g_write_buffer);
        g_write_buffer = NULL;
    }
    g_write_buffer_pos = 0;

    // Update status if not already set to ERROR
    if (status_get_int(STT_LOGGER_STATUS) != LOGGER_ERROR)
    {
        status_set(STT_LOGGER_STATUS, LOGGER_STOPPED);
    }
    status_set(STT_LOGGER_FILE, "");

    g_logger_task_handle = NULL;
    ESP_LOGI(TAG, "logger task stopped");
    vTaskDelete(NULL);
}

esp_err_t logger_start(const char* filename)
{
    if (filename == NULL)
    {
        ESP_LOGE(TAG, "filename is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (g_logger_running)
    {
        ESP_LOGW(TAG, "logger already running");
        return ESP_ERR_INVALID_STATE;
    }

    if (status_get_int(STT_GNSS_MODE) == GNSS_BASE)
    {
        ESP_LOGW(TAG, "Cannot start logger in Base mode (UART2 used by NTRIP caster)");
        return ESP_ERR_INVALID_STATE;
    }

    // Allocate DMA-capable write buffer for fast SD card writes
    g_write_buffer = heap_caps_malloc(LOGGER_WRITE_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (g_write_buffer == NULL)
    {
        ESP_LOGE(TAG, "failed to allocate DMA write buffer (%d bytes)", LOGGER_WRITE_BUFFER_SIZE);
        return ESP_ERR_NO_MEM;
    }
    g_write_buffer_pos = 0;

    g_log_file = sdcard_open(filename, "ab");
    if (g_log_file == NULL)
    {
        ESP_LOGE(TAG, "failed to open log file: %s", filename);
        heap_caps_free(g_write_buffer);
        g_write_buffer = NULL;
        return ESP_FAIL;
    }

    // Disable stdio buffering — we manage our own 32 KB buffer
    setvbuf(g_log_file, NULL, _IONBF, 0);

    g_logger_running = true;

    if (xTaskCreate(logger_task, "logger", 2048, NULL, 5, &g_logger_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "failed to create logger task");
        sdcard_close(g_log_file);
        g_log_file = NULL;
        heap_caps_free(g_write_buffer);
        g_write_buffer = NULL;
        g_logger_running = false;
        status_set(STT_LOGGER_STATUS, LOGGER_ERROR);
        return ESP_FAIL;
    }

    // Update status to indicate logger is running
    status_set(STT_LOGGER_FILE, filename);
    status_set(STT_LOGGER_STATUS, LOGGER_RUNNING);
    status_set(STT_LOGGER_SIZE, 0);

    ESP_LOGI(TAG, "started logging to %s (buffer %d KB)", filename, LOGGER_WRITE_BUFFER_SIZE / 1024);
    return ESP_OK;
}

esp_err_t logger_stop(void)
{
    if (!g_logger_running)
    {
        ESP_LOGW(TAG, "logger not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "stopping logger...");
    g_logger_running = false;

    // Wait for the logger task to finish its final flush and cleanup
    while (g_logger_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Status is already updated in logger_task cleanup
    ESP_LOGI(TAG, "logger stopped");
    return ESP_OK;
}
