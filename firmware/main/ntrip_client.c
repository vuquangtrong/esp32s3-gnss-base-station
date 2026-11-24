#include "ntrip_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "ntrip_client";

static portMUX_TYPE g_ntrip_mux = portMUX_INITIALIZER_UNLOCKED;

// GGA task state and static buffers
static TaskHandle_t g_gga_task_handle = NULL;
static volatile bool g_gga_task_stop = false;
static uint8_t g_gga_uart_buf[256];
static char g_gga_line_buf[NTRIP_NMEA_BUF_SIZE];
static char g_gga_buf[NTRIP_NMEA_BUF_SIZE];
static bool g_gga_ready = false;

// Single-caster connection state and shared static buffers
static TaskHandle_t g_stream_task_handle = NULL;
static volatile bool g_stream_stop = false;
static bool g_caster_busy = false;
static ntrip_client_cfg_t g_stream_cfg;
static char g_http_rx_buf[NTRIP_RX_BUF_SIZE];
static char g_query_line_buf[NTRIP_LINE_BUF_SIZE];
static char g_mntpoints_json[NTRIP_JSON_BUF_SIZE];

static bool ntrip_acquire_caster(void)
{
    bool acquired = false;
    taskENTER_CRITICAL(&g_ntrip_mux);
    if (!g_caster_busy && g_stream_task_handle == NULL)
    {
        g_caster_busy = true;
        acquired = true;
    }
    taskEXIT_CRITICAL(&g_ntrip_mux);
    return acquired;
}

static void ntrip_release_caster(void)
{
    taskENTER_CRITICAL(&g_ntrip_mux);
    g_caster_busy = false;
    taskEXIT_CRITICAL(&g_ntrip_mux);
}

static void ntrip_save_gga(const char* gga)
{
    if (gga == NULL || gga[0] == '\0')
    {
        return;
    }

    taskENTER_CRITICAL(&g_ntrip_mux);
    strlcpy(g_gga_buf, gga, sizeof(g_gga_buf));
    g_gga_ready = true;
    taskEXIT_CRITICAL(&g_ntrip_mux);
}

static bool ntrip_take_gga(char* out_buf, size_t out_len, bool force_if_present)
{
    bool taken = false;
    taskENTER_CRITICAL(&g_ntrip_mux);
    if (g_gga_buf[0] != '\0' && (g_gga_ready || force_if_present))
    {
        strlcpy(out_buf, g_gga_buf, out_len);
        g_gga_ready = false;
        taken = true;
    }
    taskEXIT_CRITICAL(&g_ntrip_mux);
    return taken;
}

static void ntrip_gga_task(void* arg)
{
    (void)arg;
    size_t line_idx = 0;
    bool in_sentence = false;

    ESP_LOGI(TAG, "UART2 GGA reader task started");

    while (!g_gga_task_stop)
    {
        int len = uart_read_bytes(UART2_PORT, g_gga_uart_buf, sizeof(g_gga_uart_buf), pdMS_TO_TICKS(NTRIP_UART_READ_TIMEOUT_MS));
        if (len <= 0)
        {
            continue;
        }

        for (int i = 0; i < len; i++)
        {
            char c = (char)g_gga_uart_buf[i];
            if (c == '$')
            {
                g_gga_line_buf[0] = '$';
                line_idx = 1;
                in_sentence = true;
            }
            else if (in_sentence)
            {
                if (line_idx < sizeof(g_gga_line_buf) - 1)
                {
                    g_gga_line_buf[line_idx++] = c;
                }
                else
                {
                    in_sentence = false;
                    line_idx = 0;
                    continue;
                }

                if (c == '\n')
                {
                    g_gga_line_buf[line_idx] = '\0';
                    if (strstr(g_gga_line_buf, "GGA,") != NULL)
                    {
                        ntrip_save_gga(g_gga_line_buf);
                        g_gga_line_buf[line_idx - 2] = '\0';  // remove \r\n
                        ESP_LOGI(TAG, "%s", g_gga_line_buf);
                    }
                    in_sentence = false;
                    line_idx = 0;
                }
            }
        }
    }

    ESP_LOGI(TAG, "UART2 GGA reader task stopped");
    g_gga_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ntrip_client_start_gga_task(void)
{
    if (g_gga_task_handle != NULL)
    {
        return ESP_OK;
    }

    g_gga_task_stop = false;
    if (xTaskCreate(ntrip_gga_task, "ntrip_gga", 4096, NULL, 5, &g_gga_task_handle) != pdPASS)
    {
        g_gga_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create UART2 GGA task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ntrip_client_stop_gga_task(void)
{
    if (g_gga_task_handle == NULL)
    {
        return;
    }

    g_gga_task_stop = true;
    while (g_gga_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uart_flush_input(UART2_PORT);
}

const char* ntrip_client_query_mountpoints(const char* server, uint16_t port, const char* username, const char* password)
{
    if (server == NULL || server[0] == '\0' || port == 0)
    {
        return NULL;
    }

    // Restrict to allow ONLY ONE connection to caster
    if (!ntrip_acquire_caster())
    {
        ESP_LOGW(TAG, "Caster connection busy, cannot query mountpoints");
        return NULL;
    }

    esp_http_client_config_t config = {
        .host = server,
        .port = (int)port,
        .path = "/",
        .username = (username != NULL && username[0] != '\0') ? username : NULL,
        .password = (password != NULL) ? password : NULL,
        .auth_type = (username != NULL && username[0] != '\0') ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .timeout_ms = NTRIP_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to init HTTP client for mountpoint query");
        ntrip_release_caster();
        return NULL;
    }

    esp_http_client_set_header(client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(client, "Ntrip-Version", "Ntrip/2.0");

    if (esp_http_client_open(client, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open connection to %s:%u", server, (unsigned)port);
        esp_http_client_cleanup(client);
        ntrip_release_caster();
        return NULL;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Caster returned HTTP status %d for sourcetable", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ntrip_release_caster();
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* data_arr = (root != NULL) ? cJSON_AddArrayToObject(root, "data") : NULL;
    if (data_arr == NULL)
    {
        cJSON_Delete(root);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ntrip_release_caster();
        return NULL;
    }

    size_t mnt_count = 0;
    size_t line_len = 0;
    int bytes_read = 0;

    while ((bytes_read = esp_http_client_read(client, g_http_rx_buf, sizeof(g_http_rx_buf))) > 0)
    {
        for (int i = 0; i < bytes_read; i++)
        {
            char c = g_http_rx_buf[i];
            if (c == '\r')
            {
                continue;
            }
            if (c == '\n')
            {
                g_query_line_buf[line_len] = '\0';
                if (mnt_count < NTRIP_MNTPOINTS_MAX && strncmp(g_query_line_buf, "STR;", 4) == 0)
                {
                    char* saveptr = NULL;
                    char* tok = strtok_r(g_query_line_buf, ";", &saveptr);
                    if (tok != NULL)
                    {
                        char* mnt = strtok_r(NULL, ";", &saveptr);
                        if (mnt != NULL && mnt[0] != '\0')
                        {
                            cJSON* str_item = cJSON_CreateString(mnt);
                            if (str_item != NULL)
                            {
                                cJSON_AddItemToArray(data_arr, str_item);
                                ESP_LOGI(TAG, "Mntpoint: %s", mnt);
                                mnt_count++;
                            }
                        }
                    }
                }
                line_len = 0;
            }
            else if (line_len < sizeof(g_query_line_buf) - 1)
            {
                g_query_line_buf[line_len++] = c;
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool ok = cJSON_PrintPreallocated(root, g_mntpoints_json, (int)sizeof(g_mntpoints_json), false);
    cJSON_Delete(root);
    ntrip_release_caster();

    if (!ok)
    {
        ESP_LOGE(TAG, "Failed to serialize mountpoint list");
        return NULL;
    }

    ESP_LOGI(TAG, "Queried %zu mountpoints from %s:%u", mnt_count, server, (unsigned)port);
    return g_mntpoints_json;
}

static void ntrip_stream_task(void* arg)
{
    (void)arg;
    status_set_int(STT_NTRIP_CLIENT_STATUS, CONN_CONNECTING);

    char path[NTRIP_MNTPOINT_LEN_MAX + 2];
    snprintf(path, sizeof(path), "/%s", g_stream_cfg.mntpoint);

    esp_http_client_config_t config = {
        .host = g_stream_cfg.server,
        .port = (int)g_stream_cfg.port,
        .path = path,
        .username = (g_stream_cfg.username[0] != '\0') ? g_stream_cfg.username : NULL,
        .password = g_stream_cfg.password,
        .auth_type = (g_stream_cfg.username[0] != '\0') ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
        .timeout_ms = NTRIP_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to init HTTP client for streaming");
        goto cleanup;
    }

    esp_http_client_set_header(client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(client, "Ntrip-Version", "Ntrip/2.0");
    esp_http_client_set_header(client, "Connection", "keep-alive");

    if (esp_http_client_open(client, 0) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to connect to caster %s:%u%s", g_stream_cfg.server, (unsigned)g_stream_cfg.port, path);
        goto cleanup;
    }

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Caster rejected stream connection with status %d", status_code);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Connected to caster %s:%u%s", g_stream_cfg.server, (unsigned)g_stream_cfg.port, path);
    status_set_int(STT_NTRIP_CLIENT_STATUS, CONN_CONNECTED);
    status_set_int(STT_NTRIP_RECEIVED_BYTES, 0);

    // Use a short read timeout for low-latency RTCM forwarding and responsive disconnect
    esp_http_client_set_timeout_ms(client, NTRIP_STREAM_READ_TIMEOUT_MS);

    char tx_gga[NTRIP_NMEA_BUF_SIZE];
    // Immediately send buffered GGA message upon connecting
    if (ntrip_take_gga(tx_gga, sizeof(tx_gga), true))
    {
        if (esp_http_client_write(client, tx_gga, (int)strlen(tx_gga)) < 0)
        {
            ESP_LOGW(TAG, "Failed to send initial GGA to caster");
            goto cleanup;
        }
        ESP_LOGI(TAG, "Sent initial GGA: %s", tx_gga);
    }

    uint32_t idle_ticks = 0;

    while (!g_stream_stop)
    {
        // Send again whenever a new GGA message is set
        if (ntrip_take_gga(tx_gga, sizeof(tx_gga), false))
        {
            if (esp_http_client_write(client, tx_gga, (int)strlen(tx_gga)) < 0)
            {
                ESP_LOGW(TAG, "Failed to send updated GGA to caster");
                break;
            }
            ESP_LOGI(TAG, "Sent GGA: %s", tx_gga);
        }

        int bytes_read = esp_http_client_read(client, g_http_rx_buf, sizeof(g_http_rx_buf));
        if (bytes_read > 0)
        {
            idle_ticks = 0;
            uart_write_bytes(UART2_PORT, g_http_rx_buf, (size_t)bytes_read);
            status_set_int(STT_NTRIP_RECEIVED_BYTES, status_get_int(STT_NTRIP_RECEIVED_BYTES) + bytes_read);
        }
        else if (bytes_read == -ESP_ERR_HTTP_EAGAIN)
        {
            idle_ticks++;
            if (idle_ticks >= NTRIP_STREAM_MAX_IDLE_TICKS)
            {
                ESP_LOGW(TAG, "Caster stream timed out (no data received)");
                break;
            }
        }
        else if (bytes_read == 0)
        {
            ESP_LOGI(TAG, "Caster closed connection");
            break;
        }
        else
        {
            if (!g_stream_stop)
            {
                ESP_LOGW(TAG, "Caster stream read error: %d", bytes_read);
            }
            break;
        }
    }

cleanup:
    if (client != NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    status_set_int(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
    g_stream_task_handle = NULL;
    ntrip_release_caster();
    ESP_LOGI(TAG, "NTRIP stream task stopped");
    vTaskDelete(NULL);
}

esp_err_t ntrip_client_connect(const char* server, uint16_t port, const char* mntpoint, const char* username, const char* password)
{
    if (server == NULL || server[0] == '\0' || port == 0 || mntpoint == NULL || mntpoint[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Restrict to allow ONLY ONE connection to caster
    if (!ntrip_acquire_caster())
    {
        ESP_LOGW(TAG, "Caster connection already active");
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(g_stream_cfg.server, server, sizeof(g_stream_cfg.server));
    g_stream_cfg.port = port;
    strlcpy(g_stream_cfg.mntpoint, mntpoint, sizeof(g_stream_cfg.mntpoint));
    strlcpy(g_stream_cfg.username, (username != NULL) ? username : "", sizeof(g_stream_cfg.username));
    strlcpy(g_stream_cfg.password, (password != NULL) ? password : "", sizeof(g_stream_cfg.password));

    g_stream_stop = false;
    if (xTaskCreate(ntrip_stream_task, "ntrip_stream", 8192, NULL, 5, &g_stream_task_handle) != pdPASS)
    {
        g_stream_task_handle = NULL;
        ntrip_release_caster();
        ESP_LOGE(TAG, "Failed to create NTRIP stream task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ntrip_client_disconnect(void)
{
    if (g_stream_task_handle == NULL)
    {
        return;
    }

    g_stream_stop = true;
    while (g_stream_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
