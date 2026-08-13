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

typedef enum
{
    SCAN_NONE,
    SCAN_NMEA,
    SCAN_UBX
} scan_state_t;

static TaskHandle_t g_parser_task_handle = NULL;
static nmea_parser_ctx_t g_nmea_parser_ctx = {0};
static ubx_parser_ctx_t g_ubx_parser_ctx = {0};
static scan_state_t g_scan_active = SCAN_NONE;

static char g_nmea_gga[NMEA_BUFFER_SIZE] = {0};

const char* parser_get_nmea_gga(void)
{
    return g_nmea_gga;
}

static void parser_process_nmea_gga(const char* gga_sentence)
{
    if (gga_sentence == NULL || gga_sentence[0] == '\0')
    {
        return;
    }

    strlcpy(g_nmea_gga, gga_sentence, NMEA_BUFFER_SIZE);
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

static void parser_process_data(nmea_parser_ctx_t* nmea_ctx, ubx_parser_ctx_t* ubx_ctx, const uint8_t* data, uint16_t length)
{
    if (nmea_ctx == NULL || ubx_ctx == NULL || data == NULL || length == 0)
    {
        return;
    }

    uint16_t i = 0;

    while (i < length)
    {
        switch (g_scan_active)
        {
            case SCAN_NONE:
            {
                uint8_t c = data[i];
                if (c == '$')
                {
                    g_scan_active = SCAN_NMEA;
                    nmea_ctx->buf[0] = '$';
                    nmea_ctx->idx = 1;
                    nmea_ctx->in_msg = true;
                }
                else if (c == UBX_SYNC1)
                {
                    g_scan_active = SCAN_UBX;
                    ubx_ctx->state = UBX_STATE_SYNC2;
                }
                i++;
                break;
            }

            case SCAN_NMEA:
            {
                uint8_t c = data[i];

                if (c == '$')
                {
                    // New NMEA sentence starts, restart accumulation
                    nmea_ctx->buf[0] = '$';
                    nmea_ctx->idx = 1;
                    i++;
                }
                else if (c == UBX_SYNC1)
                {
                    // 0xB5 is outside valid ASCII — this is a UBX start, not NMEA data
                    nmea_ctx->in_msg = false;
                    nmea_ctx->idx = 0;
                    g_scan_active = SCAN_NONE;
                    // Don't increment i — let SCAN pick up this 0xB5
                }
                else if (c == '\n')
                {
                    if (nmea_ctx->idx < NMEA_BUFFER_SIZE - 2)
                    {
                        nmea_ctx->buf[nmea_ctx->idx++] = '\n';
                        nmea_ctx->buf[nmea_ctx->idx] = '\0';

                        if (nmea_ctx->idx > 6 && memcmp(&nmea_ctx->buf[3], "GGA,", 4) == 0)
                        {
                            parser_process_nmea_gga((const char*)nmea_ctx->buf);
                        }
                    }
                    nmea_ctx->in_msg = false;
                    nmea_ctx->idx = 0;
                    g_scan_active = SCAN_NONE;
                    i++;
                }
                else
                {
                    if (nmea_ctx->idx < NMEA_BUFFER_SIZE - 2)
                    {
                        nmea_ctx->buf[nmea_ctx->idx++] = (char)c;
                        i++;
                    }
                    else
                    {
                        // Overflow — abandon this sentence, let SCAN re-examine this byte
                        nmea_ctx->in_msg = false;
                        nmea_ctx->idx = 0;
                        g_scan_active = SCAN_NONE;
                    }
                }
                break;
            }

            case SCAN_UBX:
            {
                switch (ubx_ctx->state)
                {
                    case UBX_STATE_SYNC2:
                        if (data[i] == UBX_SYNC2)
                        {
                            ubx_ctx->state = UBX_STATE_CLASS;
                            i++;
                        }
                        else if (data[i] == UBX_SYNC1)
                        {
                            // Another 0xB5, stay in SYNC2
                            i++;
                        }
                        else
                        {
                            // Not a valid UBX frame, let SCAN re-examine this byte
                            ubx_ctx->state = UBX_STATE_IDLE;
                            g_scan_active = SCAN_NONE;
                        }
                        break;

                    case UBX_STATE_CLASS:
                        ubx_ctx->msg_class = data[i];
                        ubx_ctx->state = UBX_STATE_ID;
                        i++;
                        break;

                    case UBX_STATE_ID:
                        ubx_ctx->msg_id = data[i];
                        ubx_ctx->state = UBX_STATE_LEN_L;
                        i++;
                        break;

                    case UBX_STATE_LEN_L:
                        ubx_ctx->payload_len = (uint16_t)data[i];
                        ubx_ctx->state = UBX_STATE_LEN_H;
                        i++;
                        break;

                    case UBX_STATE_LEN_H:
                        ubx_ctx->payload_len |= ((uint16_t)data[i] << 8);
                        ubx_ctx->payload_idx = 0;
                        i++;
                        if (ubx_ctx->payload_len > UBX_PAYLOAD_LEN_MAX)
                        {
                            // Corrupt length — abandon frame, return to scan
                            ubx_ctx->state = UBX_STATE_IDLE;
                            g_scan_active = SCAN_NONE;
                        }
                        else if (ubx_ctx->msg_class == UBX_CLASS_NAV && ubx_ctx->msg_id == UBX_ID_NAV_PVT && ubx_ctx->payload_len == sizeof(ubx_nav_pvt_t))
                        {
                            ubx_ctx->state = UBX_STATE_PAYLOAD;
                        }
                        else
                        {
                            ubx_ctx->skip_bytes_remaining = ubx_ctx->payload_len + 2;
                            if (ubx_ctx->skip_bytes_remaining == 0)
                            {
                                ubx_ctx->state = UBX_STATE_IDLE;
                                g_scan_active = SCAN_NONE;
                            }
                            else
                            {
                                ubx_ctx->state = UBX_STATE_SKIP;
                            }
                        }
                        break;

                    case UBX_STATE_PAYLOAD:
                    {
                        uint16_t bytes_needed = (uint16_t)sizeof(ubx_nav_pvt_t) - ubx_ctx->payload_idx;
                        uint16_t bytes_avail = length - i;
                        uint16_t chunk_size = (bytes_needed < bytes_avail) ? bytes_needed : bytes_avail;

                        if (chunk_size > 0)
                        {
                            memcpy(&ubx_ctx->payload[ubx_ctx->payload_idx], &data[i], chunk_size);
                            ubx_ctx->payload_idx += chunk_size;
                            i += chunk_size;
                        }

                        if (ubx_ctx->payload_idx >= sizeof(ubx_nav_pvt_t))
                        {
                            parser_process_ubx_nav_pvt((const ubx_nav_pvt_t*)ubx_ctx->payload);
                            ubx_ctx->skip_bytes_remaining = 2;
                            ubx_ctx->state = UBX_STATE_SKIP;
                        }
                        break;
                    }

                    case UBX_STATE_SKIP:
                    {
                        uint16_t bytes_avail = length - i;
                        uint16_t skip_count = (ubx_ctx->skip_bytes_remaining < bytes_avail) ? ubx_ctx->skip_bytes_remaining : bytes_avail;

                        ubx_ctx->skip_bytes_remaining -= skip_count;
                        i += skip_count;

                        if (ubx_ctx->skip_bytes_remaining == 0)
                        {
                            ubx_ctx->state = UBX_STATE_IDLE;
                            g_scan_active = SCAN_NONE;
                        }
                        break;
                    }

                    default:
                        ubx_ctx->state = UBX_STATE_IDLE;
                        g_scan_active = SCAN_NONE;
                        i++;
                        break;
                }
                break;
            }

            default:
                g_scan_active = SCAN_NONE;
                i++;
                break;
        }
    }
}

static void parser_reset(void)
{
    memset(&g_nmea_parser_ctx, 0, sizeof(g_nmea_parser_ctx));
    memset(&g_ubx_parser_ctx, 0, sizeof(g_ubx_parser_ctx));
    g_scan_active = SCAN_NONE;
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
                            parser_process_data(&g_nmea_parser_ctx, &g_ubx_parser_ctx, rx_buf, (uint16_t)len);
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
                    parser_reset();
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART1 ring buffer full");
                    uart_flush_input(UART1_PORT);
                    xQueueReset(uart1_queue);
                    parser_reset();
                    break;
                default:
                    break;
            }
        }
    }
}

esp_err_t parser_init(void)
{
    parser_reset();

    if (xTaskCreate(parser_task, "parser", 2048, NULL, 5, &g_parser_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create parser task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
