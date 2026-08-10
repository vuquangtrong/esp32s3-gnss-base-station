#include "parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "status.h"

static const char* TAG = "parser";

static QueueHandle_t g_parser_queue = NULL;
static TaskHandle_t g_parser_task_handle = NULL;
static ubx_parser_ctx_t g_parser_ctx = {0};

static void parser_process_nav_pvt(const ubx_parser_ctx_t* ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    ubx_nav_pvt_t pvt = {0};
    memcpy(&pvt, ctx->payload, sizeof(ubx_nav_pvt_t));

    char str_buf[64] = {0};

    snprintf(str_buf, sizeof(str_buf), "%04u-%02u-%02u", pvt.year, pvt.month, pvt.day);
    status_set(STT_GNSS_DATE, str_buf);

    snprintf(str_buf, sizeof(str_buf), "%02u:%02u:%02u", pvt.hour, pvt.min, pvt.sec);
    status_set(STT_GNSS_TIME, str_buf);

    status_set(STT_GNSS_LAT, (double)pvt.lat * 1e-7);
    status_set(STT_GNSS_LON, (double)pvt.lon * 1e-7);
    status_set(STT_GNSS_ALT, (double)pvt.hMSL / 1000.0);
    status_set(STT_GNSS_SAT, (int)pvt.numSV);
    status_set(STT_GNSS_HACC, (double)pvt.hAcc / 1000.0);
    status_set(STT_GNSS_VACC, (double)pvt.vAcc / 1000.0);

    const char* fix_str = "NO FIX";
    uint8_t carr_soln = (pvt.flags >> 6) & 0x03;
    bool gnss_fix_ok = (pvt.flags & 0x01) != 0;

    if (gnss_fix_ok)
    {
        if (carr_soln == 2)
        {
            fix_str = "FIXED RTK";
        }
        else if (carr_soln == 1)
        {
            fix_str = "FLOAT RTK";
        }
        else if (pvt.fixType == 3)
        {
            fix_str = "3D FIX";
        }
        else if (pvt.fixType == 2)
        {
            fix_str = "2D FIX";
        }
        else if (pvt.fixType == 4)
        {
            fix_str = "GNSS+DR";
        }
        else if (pvt.fixType == 5)
        {
            fix_str = "TIME FIX";
        }
        else if (pvt.fixType == 1)
        {
            fix_str = "DR ONLY";
        }
    }
    status_set(STT_GNSS_FIX, fix_str);
}

static void parser_process_buf(ubx_parser_ctx_t* ctx, const uint8_t* data, uint16_t length)
{
    if (ctx == NULL || data == NULL || length == 0)
    {
        return;
    }

    uint16_t i = 0;
    while (i < length)
    {
        switch (ctx->state)
        {
            case UBX_STATE_IDLE:
                if (data[i] == UBX_SYNC1)
                {
                    ctx->state = UBX_STATE_SYNC2;
                }
                i++;
                break;

            case UBX_STATE_SYNC2:
                if (data[i] == UBX_SYNC2)
                {
                    ctx->state = UBX_STATE_CLASS;
                }
                else if (data[i] == UBX_SYNC1)
                {
                    ctx->state = UBX_STATE_SYNC2;
                }
                else
                {
                    ctx->state = UBX_STATE_IDLE;
                }
                i++;
                break;

            case UBX_STATE_CLASS:
                ctx->msg_class = data[i];
                ctx->state = UBX_STATE_ID;
                i++;
                break;

            case UBX_STATE_ID:
                ctx->msg_id = data[i];
                ctx->state = UBX_STATE_LEN_L;
                i++;
                break;

            case UBX_STATE_LEN_L:
                ctx->payload_len = (uint16_t)data[i];
                ctx->state = UBX_STATE_LEN_H;
                i++;
                break;

            case UBX_STATE_LEN_H:
                ctx->payload_len |= ((uint16_t)data[i] << 8);
                ctx->payload_idx = 0;
                i++;
                if (ctx->msg_class == UBX_CLASS_NAV && ctx->msg_id == UBX_ID_NAV_PVT && ctx->payload_len == sizeof(ubx_nav_pvt_t))
                {
                    ctx->state = UBX_STATE_PAYLOAD;
                }
                else
                {
                    ctx->skip_bytes_remaining = ctx->payload_len + 2;
                    if (ctx->skip_bytes_remaining == 0)
                    {
                        ctx->state = UBX_STATE_IDLE;
                    }
                    else
                    {
                        ctx->state = UBX_STATE_SKIP;
                    }
                }
                break;

            case UBX_STATE_PAYLOAD:
            {
                uint16_t bytes_needed = (uint16_t)sizeof(ubx_nav_pvt_t) - ctx->payload_idx;
                uint16_t bytes_avail = length - i;
                uint16_t chunk_size = (bytes_needed < bytes_avail) ? bytes_needed : bytes_avail;

                if (chunk_size > 0)
                {
                    memcpy(&ctx->payload[ctx->payload_idx], &data[i], chunk_size);
                    ctx->payload_idx += chunk_size;
                    i += chunk_size;
                }

                if (ctx->payload_idx >= sizeof(ubx_nav_pvt_t))
                {
                    parser_process_nav_pvt(ctx);
                    ctx->skip_bytes_remaining = 2;
                    ctx->state = UBX_STATE_SKIP;
                }
                break;
            }

            case UBX_STATE_SKIP:
            {
                uint16_t bytes_avail = length - i;
                uint16_t skip_count = (ctx->skip_bytes_remaining < bytes_avail) ? ctx->skip_bytes_remaining : bytes_avail;

                ctx->skip_bytes_remaining -= skip_count;
                i += skip_count;

                if (ctx->skip_bytes_remaining == 0)
                {
                    ctx->state = UBX_STATE_IDLE;
                }
                break;
            }

            default:
                ctx->state = UBX_STATE_IDLE;
                i++;
                break;
        }
    }
}

static void parser_task(void* arg)
{
    uart_buf_t* buf = NULL;

    ESP_LOGI(TAG, "task started");
    while (1)
    {
        if (xQueueReceive(g_parser_queue, (void*)&buf, portMAX_DELAY) == pdTRUE)
        {
            if (buf != NULL)
            {
                if (buf->length > 0)
                {
                    parser_process_buf(&g_parser_ctx, buf->data, buf->length);
                }
                uart1_buf_release(buf);
                buf = NULL;
            }
        }
    }
}

esp_err_t parser_init(void)
{
    if (g_parser_queue != NULL)
    {
        return ESP_OK;
    }

    memset(&g_parser_ctx, 0, sizeof(g_parser_ctx));

    g_parser_queue = xQueueCreate(PARSER_QUEUE_SIZE, sizeof(uart_buf_t*));
    if (g_parser_queue == NULL)
    {
        ESP_LOGE(TAG, "failed to create queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t parser_task_start(void)
{
    if (g_parser_queue == NULL)
    {
        ESP_LOGE(TAG, "parser not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_parser_task_handle != NULL)
    {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(parser_task, "parser_task", 4096, NULL, 6, &g_parser_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "failed to create parser task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t parser_post_buf(uart_buf_t* buf)
{
    if (buf == NULL || buf->length == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_parser_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(g_parser_queue, (void*)&buf, 0) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
