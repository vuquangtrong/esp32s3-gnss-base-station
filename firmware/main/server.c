#include "server.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "config.h"
#include "driver/uart.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "server";

static httpd_handle_t g_server = NULL;
static char g_file_buf[SERVER_FILE_BUF_LEN];
static uint8_t g_uart_buf[SERVER_UART_BUF_LEN];
static volatile bool g_ws_client_joined = false;

static const char* server_content_type(const char* filepath)
{
    const char* ext = strrchr(filepath, '.');
    if (ext == NULL)
    {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0)
    {
        return "text/html";
    }
    if (strcmp(ext, ".css") == 0)
    {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0)
    {
        return "application/javascript";
    }
    if (strcmp(ext, ".ico") == 0)
    {
        return "image/x-icon";
    }
    if (strcmp(ext, ".json") == 0)
    {
        return "application/json";
    }
    return "application/octet-stream";
}

static esp_err_t server_handle_config_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, config_get_all());
}

static esp_err_t server_handle_status_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, status_get_all());
}

static esp_err_t server_handle_ws(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->method == HTTP_GET)
    {
        g_ws_client_joined = true;
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PING)
    {
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &ws_pkt);
    }

    return ESP_OK;
}

static void server_ws_broadcast(httpd_ws_type_t type, const uint8_t* payload, size_t len)
{
    if (g_server == NULL || payload == NULL || len == 0)
    {
        return;
    }

    size_t fds = SERVER_WS_CLIENTS_MAX;
    int client_fds[SERVER_WS_CLIENTS_MAX] = {0};
    if (httpd_get_client_list(g_server, &fds, client_fds) != ESP_OK)
    {
        return;
    }

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = type,
        .payload = (uint8_t*)payload,
        .len = len,
    };

    for (size_t i = 0; i < fds; i++)
    {
        if (httpd_ws_get_fd_info(g_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
        {
            httpd_ws_send_frame_async(g_server, client_fds[i], &ws_pkt);
        }
    }
}

static void server_ws_task(void* arg)
{
    while (1)
    {
        int len = uart_read_bytes(UART1_PORT, g_uart_buf, sizeof(g_uart_buf), pdMS_TO_TICKS(SERVER_UART_TIMEOUT_MS));
        if (len > 0)
        {
            server_ws_broadcast(HTTPD_WS_TYPE_BINARY, g_uart_buf, (size_t)len);
        }

        if (status_has_changed() || g_ws_client_joined)
        {
            g_ws_client_joined = false;
            const char* status_json = status_get_all();
            server_ws_broadcast(HTTPD_WS_TYPE_TEXT, (const uint8_t*)status_json, strlen(status_json));
        }
    }
}

static esp_err_t server_handle_static_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char filepath[SERVER_FILE_PATH_MAX];
    const char* uri = req->uri;
    size_t uri_len = strcspn(uri, "?#");

    if (uri_len == 0 || (uri_len == 1 && uri[0] == '/'))
    {
        snprintf(filepath, sizeof(filepath), "%s/index.html", WWW_BASE_PATH);
    }
    else
    {
        if (strstr(uri, "..") != NULL)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
            return ESP_FAIL;
        }

        int written = snprintf(filepath, sizeof(filepath), "%s%.*s", WWW_BASE_PATH, (int)uri_len, uri);
        if (written < 0 || (size_t)written >= sizeof(filepath))
        {
            httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
            return ESP_FAIL;
        }
    }

    struct stat st = {0};
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode))
    {
        ESP_LOGW(TAG, "File not found: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, server_content_type(filepath));

    ssize_t bytes_read = 0;
    while ((bytes_read = read(fd, g_file_buf, sizeof(g_file_buf))) > 0)
    {
        if (httpd_resp_send_chunk(req, g_file_buf, bytes_read) != ESP_OK)
        {
            close(fd);
            httpd_resp_sendstr_chunk(req, NULL);
            ESP_LOGE(TAG, "Failed to send file chunk: %s", filepath);
            return ESP_FAIL;
        }
    }

    close(fd);

    if (bytes_read < 0)
    {
        httpd_resp_sendstr_chunk(req, NULL);
        ESP_LOGE(TAG, "Failed to read file: %s", filepath);
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t server_mount_www(void)
{
    const char* www_label = fw_update_get_active_www();
    if (www_label == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = WWW_BASE_PATH,
        .partition_label = www_label,
        .format_if_mount_failed = false,
        .read_only = true,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount LittleFS partition '%s' at '%s' (%s)", www_label, WWW_BASE_PATH, esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "WWW partition '%s' mounted: total: %zu, used: %zu", www_label, total, used);
    }

    return ESP_OK;
}

esp_err_t server_init(void)
{
    if (g_server != NULL)
    {
        return ESP_OK;
    }

    esp_err_t ret = server_mount_www();
    if (ret != ESP_OK)
    {
        return ret;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ret = httpd_start(&g_server, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    static const httpd_uri_t uri_config_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = server_handle_config_get,
    };
    httpd_register_uri_handler(g_server, &uri_config_get);

    static const httpd_uri_t uri_status_get = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = server_handle_status_get,
    };
    httpd_register_uri_handler(g_server, &uri_status_get);

    static const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = server_handle_ws,
        .is_websocket = true,
    };
    httpd_register_uri_handler(g_server, &uri_ws);

    static const httpd_uri_t uri_static_get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = server_handle_static_get,
    };
    httpd_register_uri_handler(g_server, &uri_static_get);

    if (xTaskCreate(server_ws_task, "server_ws", 4096, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create WebSocket task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
