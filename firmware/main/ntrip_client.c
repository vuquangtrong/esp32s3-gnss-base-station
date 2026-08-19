#include "ntrip_client.h"

#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "ntrip_client";

static TaskHandle_t g_stream_task_handle = NULL;
static volatile bool g_stream_stop_flag = false;

static char g_gga_buffer[NMEA_BUFFER_SIZE] = {0};
static volatile bool g_gga_ready = false;

static char g_ntrip_mountpoints[NTRIP_MOUNTPOINTS_MAX][NTRIP_MOUNTPOINT_LEN_MAX] = {0};
static int g_ntrip_mountpoint_count = 0;

static void ntrip_client_clear_mountpoints(void)
{
    g_ntrip_mountpoint_count = 0;
}

static void ntrip_client_add_mountpoint(const char* mountpoint)
{
    if (mountpoint == NULL)
    {
        return;
    }
    if (g_ntrip_mountpoint_count < NTRIP_MOUNTPOINTS_MAX)
    {
        strlcpy(g_ntrip_mountpoints[g_ntrip_mountpoint_count], mountpoint, NTRIP_MOUNTPOINT_LEN_MAX);
        g_ntrip_mountpoint_count++;
        ESP_LOGI(TAG, "Added mountpoint: %s", mountpoint);
    }
    else
    {
        ESP_LOGW(TAG, "Mountpoint limit reached");
    }
}

void ntrip_client_set_gga(const char* gga)
{
    if (gga == NULL || strlen(gga) == 0)
    {
        return;
    }

    strlcpy(g_gga_buffer, gga, NMEA_BUFFER_SIZE);
    g_gga_ready = true;
}

const char* ntrip_client_get_mountpoints(void)
{
    static char* s_mountpoints_json_str = NULL;

    // Free previous result
    if (s_mountpoints_json_str != NULL)
    {
        free(s_mountpoints_json_str);
        s_mountpoints_json_str = NULL;
    }

    cJSON* mountpoints_array = cJSON_CreateArray();
    if (mountpoints_array == NULL)
    {
        return "{\"mountpoints\":[]}";
    }

    for (int i = 0; i < g_ntrip_mountpoint_count; i++)
    {
        cJSON_AddItemToArray(mountpoints_array, cJSON_CreateString(g_ntrip_mountpoints[i]));
    }

    cJSON* response = cJSON_CreateObject();
    cJSON_AddItemToObject(response, "mountpoints", mountpoints_array);
    s_mountpoints_json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return (s_mountpoints_json_str != NULL) ? s_mountpoints_json_str : "{\"mountpoints\":[]}";
}

static void ntrip_query_mountpoints_task(void* pvParameters)
{
    ntrip_client_args_t* args = (ntrip_client_args_t*)pvParameters;
    char* rx_buffer = NULL;
    char* line_buffer = NULL;

    esp_http_client_config_t config = {
        .host = args->host,
        .port = args->port,
        .path = "/",
        .username = args->username,
        .password = args->password,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for mountpoints");
        free(args);
        vTaskDelete(NULL);
        return;
    }

    // esp_http_client_set_header(
    //     client, "User-Agent",
    //     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36 Edg/114.0.1823.51"
    // );
    esp_http_client_set_header(client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(client, "Ntrip-Version", "Ntrip/2.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection to %s: %s", args->host, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    ntrip_client_clear_mountpoints();
    esp_http_client_fetch_headers(client);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "NTRIP caster rejected request with HTTP status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    rx_buffer = malloc(NTRIP_BUFFER_SIZE);
    line_buffer = malloc(256);
    if (!rx_buffer || !line_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate buffers for mountpoints");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(rx_buffer);
        free(line_buffer);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    size_t line_len = 0;
    int len = 0;

    while ((len = esp_http_client_read(client, rx_buffer, NTRIP_BUFFER_SIZE)) > 0)
    {
        for (int i = 0; i < len; i++)
        {
            char c = rx_buffer[i];
            if (c == '\r')
            {
                continue;
            }
            if (c == '\n')
            {
                line_buffer[line_len] = '\0';
                if (strncmp(line_buffer, "STR;", 4) == 0)
                {
                    char* saveptr = NULL;
                    char* token = strtok_r(line_buffer, ";", &saveptr);
                    if (token != NULL)
                    {
                        char* mp_name = strtok_r(NULL, ";", &saveptr);
                        if (mp_name != NULL && strlen(mp_name) > 0)
                        {
                            ntrip_client_add_mountpoint(mp_name);
                        }
                    }
                }
                line_len = 0;
            }
            else
            {
                if (line_len < 255)
                {
                    line_buffer[line_len++] = c;
                }
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(rx_buffer);
    free(line_buffer);
    free(args);
    ESP_LOGI(TAG, "Mountpoints fetch completed");
    vTaskDelete(NULL);
}

void ntrip_client_query_mountpoints(const char* host, uint16_t port, const char* username, const char* password)
{
    if (host == NULL || strlen(host) == 0 || port == 0)
    {
        ESP_LOGE(TAG, "Invalid parameters for ntrip_client_query_mountpoints");
        return;
    }

    ntrip_client_args_t* args = calloc(1, sizeof(ntrip_client_args_t));
    if (args == NULL)
    {
        ESP_LOGE(TAG, "No memory for ntrip_client_args_t");
        return;
    }

    strlcpy(args->host, host, NTRIP_HOST_LEN_MAX);
    args->port = port;
    if (username != NULL)
    {
        strlcpy(args->username, username, NTRIP_USER_LEN_MAX);
    }
    if (password != NULL)
    {
        strlcpy(args->password, password, NTRIP_PASS_LEN_MAX);
    }

    xTaskCreate(ntrip_query_mountpoints_task, "ntrip_get_mp", 8192, args, 5, NULL);
}

static void ntrip_stream_task(void* pvParameters)
{
    ntrip_client_args_t* args = (ntrip_client_args_t*)pvParameters;
    char* rx_buffer = NULL;

    status_set(STT_NTRIP_CLIENT_STATUS, CONN_CONNECTING);

    // path is "/" + mountpoint, so we need to prepend a slash
    char path[NTRIP_MOUNTPOINT_LEN_MAX + 2] = {0};
    snprintf(path, sizeof(path), "/%s", args->mountpoint);

    esp_http_client_config_t config = {
        .host = args->host,
        .port = args->port,
        .path = path,
        .username = args->username,
        .password = args->password,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for streaming");
        free(args);
        status_set(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // esp_http_client_set_header(
    //     client, "User-Agent",
    //     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/114.0.0.0 Safari/537.36 Edg/114.0.1823.51"
    // );
    esp_http_client_set_header(client, "User-Agent", "NTRIP GNSS/1.0");
    esp_http_client_set_header(client, "Ntrip-Version", "Ntrip/2.0");
    esp_http_client_set_header(client, "Connection", "keep-alive");  // Must keep alive for streaming

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP streaming connection to %s: %s", args->host, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(args);
        status_set(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_fetch_headers(client);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "NTRIP caster rejected request with HTTP status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(args);
        status_set(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "NTRIP stream connected successfully to mountpoint %s", args->mountpoint);
    status_set(STT_NTRIP_CLIENT_STATUS, CONN_CONNECTED);

    rx_buffer = heap_caps_malloc(NTRIP_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!rx_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate DMA buffer for streaming");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(args);
        status_set(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
        g_stream_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    status_set(STT_NTRIP_RECEIVED_BYTES, 0);

    while (!g_stream_stop_flag)
    {
        if (g_gga_ready)
        {
            esp_http_client_write(client, g_gga_buffer, (int)strlen(g_gga_buffer));
            ESP_LOGI(TAG, "Sent: %s", g_gga_buffer);
            g_gga_ready = false;
        }

        // Read RTCM3 data from NTRIP caster and send it to UBlox UART2
        int bytes_read = esp_http_client_read(client, rx_buffer, NTRIP_BUFFER_SIZE);
        if (bytes_read > 0)
        {
            uart2_send_data((const uint8_t*)rx_buffer, (size_t)bytes_read);
            status_set(STT_NTRIP_RECEIVED_BYTES, status_get_int(STT_NTRIP_RECEIVED_BYTES) + bytes_read);
        }
        else if (bytes_read == 0)
        {
            ESP_LOGI(TAG, "NTRIP stream closed by server");
            break;
        }
        else
        {
            if (!g_stream_stop_flag)
            {
                ESP_LOGW(TAG, "HTTP read error: %d", bytes_read);
            }
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(rx_buffer);
    free(args);
    status_set(STT_NTRIP_CLIENT_STATUS, CONN_DISCONNECTED);
    g_stream_task_handle = NULL;
    ESP_LOGI(TAG, "NTRIP stream task stopped");
    vTaskDelete(NULL);
}

void ntrip_client_connect_stream(const char* host, uint16_t port, const char* mountpoint, const char* username, const char* password)
{
    if (host == NULL || strlen(host) == 0 || port == 0 || mountpoint == NULL || strlen(mountpoint) == 0)
    {
        ESP_LOGE(TAG, "Invalid parameters for ntrip_client_connect_stream");
        return;
    }

    if (g_stream_task_handle != NULL)
    {
        ntrip_client_disconnect_stream();
    }

    ntrip_client_args_t* args = calloc(1, sizeof(ntrip_client_args_t));
    if (args == NULL)
    {
        ESP_LOGE(TAG, "No memory for ntrip_client_args_t");
        return;
    }

    strlcpy(args->host, host, NTRIP_HOST_LEN_MAX);
    args->port = port;
    strlcpy(args->mountpoint, mountpoint, NTRIP_MOUNTPOINT_LEN_MAX);
    if (username != NULL)
    {
        strlcpy(args->username, username, NTRIP_USER_LEN_MAX);
    }
    if (password != NULL)
    {
        strlcpy(args->password, password, NTRIP_PASS_LEN_MAX);
    }

    g_stream_stop_flag = false;
    xTaskCreate(ntrip_stream_task, "ntrip_stream", 8192, args, 5, &g_stream_task_handle);
}

void ntrip_client_disconnect_stream(void)
{
    g_stream_stop_flag = true;
}
