#include "logger.h"

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "logger";

static QueueHandle_t g_logger_queue = NULL;
static TaskHandle_t g_logger_task_handle = NULL;

static void logger_task(void* arg)
{
    uart_buf_t* buf = NULL;

    ESP_LOGI(TAG, "task started");
    while (1)
    {
        if (xQueueReceive(g_logger_queue, (void*)&buf, portMAX_DELAY) == pdTRUE)
        {
            if (buf != NULL)
            {
                if (buf->length > 0)
                {
                    ESP_LOG_BUFFER_HEXDUMP(TAG, buf->data, buf->length, ESP_LOG_DEBUG);
                }
                uart1_buf_release(buf);
                buf = NULL;
            }
        }
    }
}

esp_err_t logger_init(void)
{
    if (g_logger_queue != NULL)
    {
        return ESP_OK;
    }

    g_logger_queue = xQueueCreate(LOGGER_QUEUE_SIZE, sizeof(uart_buf_t*));
    if (g_logger_queue == NULL)
    {
        ESP_LOGE(TAG, "failed to create queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t logger_task_start(void)
{
    if (g_logger_queue == NULL)
    {
        ESP_LOGE(TAG, "logger not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_logger_task_handle != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(logger_task, "logger_task", 2048, NULL, 5, &g_logger_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "failed to create logger task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t logger_post_buf(uart_buf_t* buf)
{
    if (buf == NULL || buf->length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_logger_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(g_logger_queue, (void*)&buf, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
