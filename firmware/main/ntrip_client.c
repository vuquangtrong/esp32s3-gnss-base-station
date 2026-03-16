#include "ntrip_client.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "ping.h"
#include "status.h"
#include "uart.h"
#include "util.h"

#define BUFFER_SIZE       2048
#define SOURCE_TABLE_SIZE 1024

static const char* TAG = "NTRIP_CLIENT";
static char* source_table;
static esp_http_client_handle_t ntrip_client = NULL;
static bool isRequestedDisconnect = false;

esp_err_t ntrip_client_init()
{
    esp_err_t err = ESP_OK;

    source_table = calloc(SOURCE_TABLE_SIZE, sizeof(char));
    ERROR_IF(source_table == NULL, return ESP_ERR_NO_MEM, "Cannot allocate source table");
    source_table[0] = '0';   // indicate that source table is not valid
    source_table[1] = '\r';  // indicate that source table is not valid
    source_table[2] = '\0';

    return err;
}

char* ntrip_client_source_table()
{
    return source_table;
}

static void ntrip_client_get_mnts_task(void* args)
{
    char* host = config_get(CONFIG_NTRIP_IP);
    int port = atoi(config_get(CONFIG_NTRIP_PORT));
    if (!port)
        port = 2101;
    char* user = config_get(CONFIG_NTRIP_USER);
    char* pwd = config_get(CONFIG_NTRIP_PWD);

    // ping_test(host);

    esp_http_client_config_t config = {
        .host = host,
        .port = port,
        .path = "/",
        .username = user,
        .password = pwd,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        // .disable_auto_redirect = true,
        // .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(client, "Ntrip-Version", "Ntrip/2.0");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_open(client, 0);
    ERROR_IF(err != ESP_OK, goto ntrip_client_get_mnts_end, "Cannot open %s:%d", host, port);

    int32_t content_length = esp_http_client_fetch_headers(client);
    if (content_length > 0)
    {
        char* buffer = calloc(content_length + 1, sizeof(char));
        ERROR_IF(buffer == NULL, goto ntrip_client_get_mnts_end, "Cannot allocate HTTP buffer");

        int32_t len = esp_http_client_read_response(client, buffer, content_length);
        if (len > 0)
        {
            buffer[len] = '\0';

            // process source table
            char** table = calloc(100, sizeof(char*));
            if (table == NULL)
            {
                ESP_LOGE(TAG, "Cannot allocate source table parser buffer");
                free(buffer);
                goto ntrip_client_get_mnts_end;
            }

            char* p = buffer;
            int n = 0;
            while (n < 100 && (p = strstr(p, "STR;")) != NULL)
            {
                p += 4;  // skip STR;
                char* s = strstr(p, ";");
                if (s == NULL)
                {
                    break;
                }
                *s = '\0';  // terminate string
                table[n++] = p;
                p = s + 1;
            }

            source_table[0] = '0';  // not valid
            source_table[1] = '\r';
            source_table[2] = '\0';
            p = source_table + 2;
            size_t remaining = SOURCE_TABLE_SIZE - 2;
            for (int i = 0; i < n; i++)
            {
                int written = snprintf(p, remaining, "%s\r", table[i]);
                if (written < 0 || (size_t)written >= remaining)
                {
                    ESP_LOGW(TAG, "Source table is too large, truncated at %d mount points", i);
                    break;
                }

                p += written;
                remaining -= (size_t)written;
            }

            source_table[0] = '1';  // valid now
            free(table);
        }
        else
        {
            ESP_LOGE(TAG, "Cannot read data from %s:%d", host, port);
        }

        free(buffer);
    }
    else
    {
        ESP_LOGE(TAG, "Cannot fetch data from %s:%d", host, port);
    }

ntrip_client_get_mnts_end:
    if (client != NULL)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    ESP_LOGI(TAG, "Finish ntrip_get_mnts!");
    vTaskDelete(NULL);
}

void ntrip_client_get_mnts()
{
    xTaskCreate(ntrip_client_get_mnts_task, "ntrip_get_mnts", 8192, NULL, 10, NULL);
}

static void uart_status_read_event_handler(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (ntrip_client != NULL)
    {
        int sent = esp_http_client_write(ntrip_client, (char*)event_data, event_id);
        ERROR_IF(sent < 0, return, "Cannot write to ntrip caster");
    }
}

static void ntrip_client_stream_task(void* args)
{
    status_set(STATUS_NTRIP_CLI_STATUS, "Connecting");

    char* host = config_get(CONFIG_NTRIP_IP);
    int port = atoi(config_get(CONFIG_NTRIP_PORT));
    if (!port)
        port = 2101;
    char* user = config_get(CONFIG_NTRIP_USER);
    char* pwd = config_get(CONFIG_NTRIP_PWD);
    char* mnt = config_get(CONFIG_NTRIP_MNT);

    char path[128];
    int path_len = snprintf(path, sizeof(path), "/%s", mnt != NULL ? mnt : "");
    ERROR_IF(path_len < 0 || path_len >= (int)sizeof(path), goto ntrip_client_stream_task_end, "NTRIP mountpoint is too long");

    // ping_test(host);

    esp_http_client_config_t config = {
        .host = host,
        .port = port,
        .path = path,
        .username = user,
        .password = pwd,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        // .disable_auto_redirect = true,
        // .timeout_ms = 30000,
    };

    ntrip_client = esp_http_client_init(&config);
    esp_http_client_set_header(ntrip_client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(ntrip_client, "Ntrip-Version", "Ntrip/2.0");
    esp_http_client_set_header(ntrip_client, "Connection", "keep-alive");

    esp_err_t err = esp_http_client_open(ntrip_client, 0);
    ERROR_IF(err != ESP_OK, goto ntrip_client_stream_task_end, "Cannot open %s:%d", host, port);

    uart_register_handler(UART_STATUS_EVENT_READ, uart_status_read_event_handler);

    int32_t content_length = esp_http_client_fetch_headers(ntrip_client);
    ERROR_IF(content_length < 0, goto ntrip_client_stream_task_end, "Cannot read from %s:%d", host, port);

    int status_code = esp_http_client_get_status_code(ntrip_client);
    ERROR_IF(status_code != 200, goto ntrip_client_stream_task_end, "Cannot get OK status from %s:%d", host, port);

    ERROR_IF(!esp_http_client_is_chunked_response(ntrip_client), goto ntrip_client_stream_task_end, "Cannot open stream to %s:%d", host, port);

    status_set(STATUS_NTRIP_CLI_STATUS, "Connected");

    char* buffer = malloc(BUFFER_SIZE);
    int len;
    while ((len = esp_http_client_read(ntrip_client, buffer, BUFFER_SIZE)) >= 0)
    {
        ubx_write_rtcm3(buffer, len);
        if (isRequestedDisconnect)
            break;
    }

    free(buffer);
    uart_unregister_handler(UART_STATUS_EVENT_READ, uart_status_read_event_handler);

ntrip_client_stream_task_end:
    if (ntrip_client != NULL)
    {
        esp_http_client_close(ntrip_client);
        esp_http_client_cleanup(ntrip_client);
    }
    ntrip_client = NULL;

    ESP_LOGI(TAG, "Finish ntrip_stream_task!");
    status_set(STATUS_NTRIP_CLI_STATUS, "Disconnected");

    vTaskDelete(NULL);
}

void ntrip_client_connect()
{
    isRequestedDisconnect = false;
    xTaskCreate(ntrip_client_stream_task, "ntrip_stream_task", 8192, NULL, 10, NULL);
}

void ntrip_client_disconnect()
{
    isRequestedDisconnect = true;
}
