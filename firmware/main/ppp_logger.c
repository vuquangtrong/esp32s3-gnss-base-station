#include "ppp_logger.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "ppp_logger";

// Static runtime state:
// - g_task sleeps until ppp_logger_start() wakes it via task notification.
// - g_file is non-NULL while a logging session is active and owned by g_task.
// - g_stop_done is signalled by g_task once the session's final flush and file close complete.
static TaskHandle_t g_task = NULL;
static SemaphoreHandle_t g_stop_done = NULL;
static FILE* volatile g_file = NULL;
static volatile bool g_stop_requested = false;

// Accumulates raw UART2 bytes in RAM so SD card writes happen in large blocks
static uint8_t g_buffer[PPP_LOGGER_BUFFER_SIZE];

static bool ppp_logger_valid_filename(const char* filename)
{
    if (filename == NULL)
    {
        return false;
    }

    // Require a non-empty base name ending with ".ubx" and reject ".." traversal
    size_t len = strlen(filename);
    if (len <= 4 || len >= PPP_LOGGER_FILENAME_MAX ||
        strcasecmp(&filename[len - 4], ".ubx") != 0 ||
        strstr(filename, "..") != NULL)
    {
        return false;
    }

    // Allow only plain filename characters (also rejects '/' and '\' path separators)
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)filename[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.')
        {
            return false;
        }
    }

    return true;
}

static bool ppp_logger_flush(FILE* file, size_t* buffered, size_t* total)
{
    if (file == NULL || buffered == NULL || total == NULL)
    {
        return false;
    }
    if (*buffered == 0)
    {
        return true;
    }

    // Write the buffered block to the SD card and saturate reported status size at INT32_MAX
    size_t written = sdcard_file_write(file, g_buffer, *buffered);
    *total += written;
    status_set_int(STT_LOGGER_SIZE, (*total > (size_t)INT32_MAX) ? INT32_MAX : (int32_t)*total);

    if (written != *buffered)
    {
        ESP_LOGE(TAG, "Failed to write log data (%zu of %zu bytes)", written, *buffered);
        return false;
    }

    *buffered = 0;
    return true;
}

static void ppp_logger_task(void* arg)
{
    (void)arg;

    for (;;)
    {
        // Park until ppp_logger_start() opens a file and notifies this task
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        FILE* file = g_file;
        if (file == NULL)
        {
            continue;
        }

        size_t buffered = 0;
        size_t total = 0;
        TickType_t last_flush = xTaskGetTickCount();
        bool error = false;

        while (!g_stop_requested)
        {
            // Stop automatically if GNSS leaves PPP mode or the SD card is unmounted
            if (status_get_int(STT_GNSS_MODE) != GNSS_PPP ||
                status_get_int(STT_SDCARD_STATUS) != SDCARD_MOUNTED ||
                status_get_int(STT_LOGGER_STATUS) != LOGGER_RUNNING)
            {
                ESP_LOGW(TAG, "Logging conditions changed, stopping: %s", status_get_str(STT_LOGGER_FILE));
                break;
            }

            // Read UART2 bytes directly into the remaining space of g_buffer in small chunks
            // so the read timeout stays short and stop requests are handled promptly
            size_t space = sizeof(g_buffer) - buffered;
            size_t to_read = (space < PPP_LOGGER_CHUNK_SIZE) ? space : PPP_LOGGER_CHUNK_SIZE;
            int len = uart_read_bytes(UART2_PORT, g_buffer + buffered, to_read, pdMS_TO_TICKS(PPP_LOGGER_UART_TIMEOUT_MS));
            if (len > 0)
            {
                buffered += (size_t)len;
            }

            // Flush when the 32 KB buffer fills up or the 10-second flush interval elapses
            TickType_t now = xTaskGetTickCount();
            if (buffered == sizeof(g_buffer) || (now - last_flush) >= pdMS_TO_TICKS(PPP_LOGGER_FLUSH_INTERVAL_MS))
            {
                if (!ppp_logger_flush(file, &buffered, &total))
                {
                    error = true;
                    break;
                }
                last_flush = now;
            }
        }

        // Flush any remaining partial block before closing the file
        if (!error && !ppp_logger_flush(file, &buffered, &total))
        {
            error = true;
        }

        if (sdcard_file_close(file) != ESP_OK)
        {
            error = true;
        }

        // Mark session inactive before waking a waiting ppp_logger_stop() caller
        g_file = NULL;
        status_set_int(STT_LOGGER_STATUS, error ? LOGGER_ERROR : LOGGER_STOPPED);
        xSemaphoreGive(g_stop_done);
    }
}

esp_err_t ppp_logger_init(void)
{
    if (g_task != NULL)
    {
        return ESP_OK;
    }

    g_stop_done = xSemaphoreCreateBinary();
    if (g_stop_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ppp_logger_task, "ppp_logger", 4096, NULL, 5, &g_task) != pdPASS)
    {
        vSemaphoreDelete(g_stop_done);
        g_stop_done = NULL;
        ESP_LOGE(TAG, "Failed to create logger task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ppp_logger_start(const char* filename)
{
    if (!ppp_logger_valid_filename(filename))
    {
        ESP_LOGE(TAG, "Invalid log filename");
        return ESP_ERR_INVALID_ARG;
    }

    // Reject if not initialized, already logging, not in PPP mode, or SD card is unavailable
    if (g_task == NULL || g_stop_done == NULL || g_file != NULL ||
        status_get_int(STT_GNSS_MODE) != GNSS_PPP ||
        status_get_int(STT_SDCARD_STATUS) != SDCARD_MOUNTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    FILE* file = sdcard_file_open(filename, "wb");
    if (file == NULL)
    {
        status_set_int(STT_LOGGER_STATUS, LOGGER_ERROR);
        return ESP_FAIL;
    }

    // Discard stale UART2 bytes and any completion token from a previous session
    uart_flush_input(UART2_PORT);
    xSemaphoreTake(g_stop_done, 0);
    g_stop_requested = false;
    g_file = file;

    status_set_str(STT_LOGGER_FILE, filename);
    status_set_int(STT_LOGGER_SIZE, 0);
    status_set_int(STT_LOGGER_STATUS, LOGGER_RUNNING);

    // Wake ppp_logger_task() to begin streaming UART2 data into g_file
    xTaskNotifyGive(g_task);
    return ESP_OK;
}

esp_err_t ppp_logger_stop(void)
{
    if (g_task == NULL || g_stop_done == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_file == NULL)
    {
        return ESP_OK;
    }

    // Signal ppp_logger_task() to exit its read loop and wait until the file is flushed and closed
    g_stop_requested = true;
    if (xSemaphoreTake(g_stop_done, pdMS_TO_TICKS(PPP_LOGGER_STOP_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Timed out waiting for logger to stop");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
