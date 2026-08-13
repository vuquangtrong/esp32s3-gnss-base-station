#include "parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "parser";

static TaskHandle_t g_parser_task_handle = NULL;
static nmea_parser_ctx_t g_nmea_parser_ctx = {0};
static ubx_parser_ctx_t g_ubx_parser_ctx = {0};

static char g_nmea_gga[NMEA_BUFFER_SIZE] = {0};

const char* parser_get_nmea_gga(void)
{
    return g_nmea_gga;
}

static void parser_process_nmea_gga(const char* gga_sentence)
{
    if (gga_sentence == NULL || strlen(gga_sentence) == 0)
    {
        return;
    }

    strlcpy(g_nmea_gga, gga_sentence, NMEA_BUFFER_SIZE);
}

static void parser_process_nmea(nmea_parser_ctx_t* ctx, const uint8_t* data, uint16_t length)
{
    if (ctx == NULL || data == NULL || length == 0)
    {
        return;
    }

    for (uint16_t i = 0; i < length; i++)
    {
        uint8_t c = data[i];
        if (c == '$')
        {
            ctx->buf[0] = '$';
            ctx->idx = 1;
            ctx->in_msg = true;
        }
        else if (ctx->in_msg)
        {
            if (c == '\n')
            {
                if (ctx->idx < NMEA_BUFFER_SIZE - 2)
                {
                    ctx->buf[ctx->idx++] = '\n';
                    ctx->buf[ctx->idx] = '\0';

                    if (strstr(ctx->buf, "GGA,") != NULL)
                    {
                        parser_process_nmea_gga((const char*)ctx->buf);
                    }
                }
                ctx->in_msg = false;
                ctx->idx = 0;
            }
            else
            {
                if (ctx->idx < NMEA_BUFFER_SIZE - 2)
                {
                    ctx->buf[ctx->idx++] = (char)c;
                }
                else
                {
                    ctx->in_msg = false;
                    ctx->idx = 0;
                }
            }
        }
    }
}

static void parser_process_ubx_nav_pvt(const ubx_nav_pvt_t* pvt)
{
    if (pvt == NULL)
    {
        return;
    }

    char str_buf[64] = {0};

    snprintf(str_buf, sizeof(str_buf), "%04u-%02u-%02uT%02u:%02u:%02u", pvt->year, pvt->month, pvt->day, pvt->hour, pvt->min, pvt->sec);
    status_set(STT_GNSS_TIME, str_buf);
    status_set(STT_GNSS_LAT, (double)pvt->lat * 1e-7);
    status_set(STT_GNSS_LON, (double)pvt->lon * 1e-7);
    status_set(STT_GNSS_ALT, (double)pvt->hMSL / 1000.0);
    status_set(STT_GNSS_SAT, (int)pvt->numSV);
    status_set(STT_GNSS_HACC, (double)pvt->hAcc / 1000.0);
    status_set(STT_GNSS_VACC, (double)pvt->vAcc / 1000.0);

    const char* fix_str = "NO FIX";
    uint8_t carr_soln = (pvt->flags >> 6) & 0x03;
    bool gnss_fix_ok = (pvt->flags & 0x01) != 0;

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
        else if (pvt->fixType == 3)
        {
            fix_str = "3D FIX";
        }
        else if (pvt->fixType == 2)
        {
            fix_str = "2D FIX";
        }
        else if (pvt->fixType == 4)
        {
            fix_str = "GNSS+DR";
        }
        else if (pvt->fixType == 5)
        {
            fix_str = "TIME FIX";
        }
        else if (pvt->fixType == 1)
        {
            fix_str = "DR ONLY";
        }
    }
    status_set(STT_GNSS_FIX, fix_str);
}

static void parser_process_ubx(ubx_parser_ctx_t* ctx, const uint8_t* data, uint16_t length)
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
                    parser_process_ubx_nav_pvt((const ubx_nav_pvt_t*)ctx->payload);
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
    (void)arg;
    QueueHandle_t uart1_queue = uart1_get_event_queue();
    if (uart1_queue == NULL)
    {
        ESP_LOGE(TAG, "UART1 event queue is NULL");
        vTaskDelete(NULL);
        return;
    }

    uart_event_t event = {0};
    static uint8_t rx_buf[256] = {0};

    while (1)
    {
        if (xQueueReceive(uart1_queue, (void*)&event, portMAX_DELAY) == pdTRUE)
        {
            switch (event.type)
            {
                case UART_DATA:
                {
                    size_t buffered_len = 0;
                    uart_get_buffered_data_len(UART1_PORT, &buffered_len);
                    while (buffered_len > 0)
                    {
                        size_t read_len = (buffered_len > sizeof(rx_buf)) ? sizeof(rx_buf) : buffered_len;
                        int len = uart_read_bytes(UART1_PORT, rx_buf, read_len, pdMS_TO_TICKS(100));
                        if (len > 0)
                        {
                            parser_process_nmea(&g_nmea_parser_ctx, rx_buf, (uint16_t)len);
                            parser_process_ubx(&g_ubx_parser_ctx, rx_buf, (uint16_t)len);
                        }
                        else
                        {
                            break;
                        }
                        uart_get_buffered_data_len(UART1_PORT, &buffered_len);
                    }
                    break;
                }
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART1 hw fifo overflow");
                    uart_flush_input(UART1_PORT);
                    xQueueReset(uart1_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART1 ring buffer full");
                    uart_flush_input(UART1_PORT);
                    xQueueReset(uart1_queue);
                    break;
                default:
                    break;
            }
        }
    }
}

esp_err_t parser_init(void)
{
    memset(&g_nmea_parser_ctx, 0, sizeof(g_nmea_parser_ctx));
    memset(&g_ubx_parser_ctx, 0, sizeof(g_ubx_parser_ctx));

    if (xTaskCreate(parser_task, "parser", 2048, NULL, 5, &g_parser_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create parser task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
