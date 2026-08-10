#include "server.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "status.h"
#include "wifi.h"

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static const char* TAG = "server";

static etag_cache_entry_t g_etag_cache[ETAG_CACHE_MAX];
static int g_etag_cache_count = 0;

static void mdns_start(void)
{
    mdns_init();
    mdns_hostname_set(MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE_NAME);

    mdns_txt_item_t serviceTxtData[] = {{"host", MDNS_HOST_NAME}, {"ip", "192.168.4.1"}};
    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData, sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));

    esp_netif_t* ap_netif = wifi_get_ap_netif();
    if (ap_netif != NULL)
    {
        ESP_LOGI(TAG, "Enabling mDNS on AP interface");
        ESP_ERROR_CHECK(mdns_netif_action(ap_netif, MDNS_EVENT_ENABLE_IP4));
    }

    esp_netif_t* sta_netif = wifi_get_sta_netif();
    if (sta_netif != NULL)
    {
        ESP_LOGI(TAG, "Enabling mDNS on STA interface");
        ESP_ERROR_CHECK(mdns_netif_action(sta_netif, MDNS_EVENT_ENABLE_IP4));
    }
}

static esp_err_t www_fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = WWW_FS_BASE_PATH,
        .partition_label = WWW_FS_PARTITION_LABEL,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

static inline int hex_to_int(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return 0;
}

static void url_decode(char* str)
{
    int len = strlen(str);
    int j = 0;
    for (int i = 0; i < len; i++)
    {
        if (str[i] == '%' && i + 2 < len)
        {
            int hex_val = (hex_to_int(str[i + 1]) << 4) | hex_to_int(str[i + 2]);
            str[j++] = (char)hex_val;
            i += 2;
        }
        else
        {
            str[j++] = str[i];
        }
    }
    str[j] = '\0';
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t* req, const char* filepath)
{
    const char* type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html"))
    {
        type = "text/html";
    }
    else if (CHECK_FILE_EXTENSION(filepath, ".css"))
    {
        type = "text/css";
    }
    else if (CHECK_FILE_EXTENSION(filepath, ".js"))
    {
        type = "application/javascript";
    }
    else if (CHECK_FILE_EXTENSION(filepath, ".ico"))
    {
        type = "image/x-icon";
    }
    else if (CHECK_FILE_EXTENSION(filepath, ".png"))
    {
        type = "image/png";
    }
    else if (CHECK_FILE_EXTENSION(filepath, ".svg"))
    {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

/* Read the ETag from cache or load from .crc file */
static bool get_file_etag(const char* filepath, char* etag_out, size_t max_len)
{
    // Check if the ETag is already in the RAM cache
    for (int i = 0; i < g_etag_cache_count; i++)
    {
        if (strncmp(g_etag_cache[i].filepath, filepath, SERVER_FILE_PATH_MAX) == 0)
        {
            strlcpy(etag_out, g_etag_cache[i].etag, max_len);
            ESP_LOGI(TAG, "ETag cache hit for %s: %s", filepath, etag_out);
            return true;
        }
    }

    // Cache miss — read CRC from filesystem
    static char crc_filepath[SERVER_FILE_PATH_MAX];
    strlcpy(crc_filepath, filepath, sizeof(crc_filepath));
    strlcat(crc_filepath, ".crc", sizeof(crc_filepath));

    int crc_fd = open(crc_filepath, O_RDONLY, 0);
    if (crc_fd == -1)
    {
        return false;  // CRC file not found
    }

    static char crc_buf[32];
    crc_buf[0] = '\0';
    ssize_t bytes_read = read(crc_fd, crc_buf, sizeof(crc_buf) - 1);
    close(crc_fd);

    if (bytes_read > 0)
    {
        // HTTP ETags must be wrapped in double quotes
        snprintf(etag_out, max_len, "\"%s\"", crc_buf);

        // Store in cache if space allows
        if (g_etag_cache_count < ETAG_CACHE_MAX)
        {
            strlcpy(g_etag_cache[g_etag_cache_count].filepath, filepath, SERVER_FILE_PATH_MAX);
            strlcpy(g_etag_cache[g_etag_cache_count].etag, etag_out, sizeof(g_etag_cache[g_etag_cache_count].etag));
            g_etag_cache_count++;
        }
        return true;
    }

    return false;  // File was empty or failed to read
}

/* Handler for GET /config */
static esp_err_t server_config_get_handler(httpd_req_t* req)
{
    const char* config_json = config_get_all();
    if (config_json == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Config not available");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, config_json, strlen(config_json));

    // ESP_LOGI(TAG, "config: %s", config_json);
    return ESP_OK;
}

/* Handler for GET /status */
static esp_err_t server_status_get_handler(httpd_req_t* req)
{
    const char* status_json = status_get_all();
    if (status_json == NULL)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status not available");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, status_json, strlen(status_json));

    // ESP_LOGI(TAG, "status: %s", status_json);
    return ESP_OK;
}

/* Handler for POST /wifi */
static esp_err_t server_wifi_post_handler(httpd_req_t* req)
{
    char query_str[256] = {0};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;

    if (query_len > sizeof(query_str))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query string too long");
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, query_str, query_len) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query parameters");
        return ESP_FAIL;
    }

    char command[32] = {0};
    if (httpd_query_key_value(query_str, "command", command, sizeof(command)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command parameter");
        return ESP_FAIL;
    }

    if (strcmp(command, "connect") == 0)
    {
        char ssid[33] = {0};
        char password[65] = {0};

        if (httpd_query_key_value(query_str, "ssid", ssid, sizeof(ssid)) != ESP_OK)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID parameter");
            return ESP_FAIL;
        }

        if (httpd_query_key_value(query_str, "password", password, sizeof(password)) != ESP_OK)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing password parameter");
            return ESP_FAIL;
        }

        url_decode(ssid);
        url_decode(password);

        ESP_LOGI(TAG, "connect SSID=%s, password=%s", ssid, password);

        esp_err_t err = wifi_sta_connect(ssid, password);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "connection failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to initiate WiFi connection");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "connection initiated");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "WiFi connection initiated", strlen("WiFi connection initiated"));
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command parameter");
    return ESP_FAIL;
}

/* Handler for GET files */
static esp_err_t server_file_get_handler(httpd_req_t* req)
{
    char filepath[SERVER_FILE_PATH_MAX];

    server_context_t* server_context = (server_context_t*)req->user_ctx;
    strlcpy(filepath, server_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/')
    {
        strlcat(filepath, "/index.html", sizeof(filepath));
    }
    else
    {
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    // fetch ETag from if exists
    char etag[64] = {0};
    bool has_etag = get_file_etag(filepath, etag, sizeof(etag));

    // process the ETag and If-None-Match header
    if (has_etag)
    {
        char client_etag[64];
        esp_err_t err = httpd_req_get_hdr_value_str(req, "If-None-Match", client_etag, sizeof(client_etag));
        if (err == ESP_OK)
        {
            // if matched, send 304 Not Modified and skip sending the file
            if (strncmp(client_etag, etag, sizeof(client_etag)) == 0)
            {
                ESP_LOGI(TAG, "ETag matched for %s. Sending 304.", filepath);
                httpd_resp_set_status(req, "304 Not Modified");
                httpd_resp_send(req, NULL, 0);
                return ESP_OK;
            }
        }
        // if not match, attach the ETag header to the upcoming file response
        httpd_resp_set_hdr(req, "ETag", etag);
    }

    // serve the actual requested file
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1)
    {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char* chunk = server_context->buffer;
    ssize_t read_bytes;
    do
    {
        // read file in chunks into the buffer
        read_bytes = read(fd, chunk, SERVER_BUFFER_SIZE);
        if (read_bytes == -1)
        {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        }
        else if (read_bytes > 0)
        {
            // send the buffer contents as HTTP response chunk
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK)
            {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                // abort sending file
                httpd_resp_sendstr_chunk(req, NULL);
                // respond with 500 Internal Server Error
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);

    // close file after sending complete
    close(fd);
    ESP_LOGI(TAG, "File %s sending complete", filepath);

    // respond with an empty chunk to signal HTTP response completion
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t server_start(const char* base_path)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(base_path && strlen(base_path) < ESP_VFS_PATH_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid base path");

    ESP_LOGI(TAG, "Initializing server context with base path: %s", base_path);
    server_context_t* server_context = calloc(1, sizeof(server_context_t));
    ESP_RETURN_ON_FALSE(server_context, ESP_ERR_NO_MEM, TAG, "No memory for rest context");
    strlcpy(server_context->base_path, base_path, sizeof(server_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    ESP_GOTO_ON_ERROR(httpd_start(&server, &config), err, TAG, "Failed to start http server");

    httpd_uri_t config_uri = {
        .uri = "/config",  //
        .method = HTTP_GET,
        .handler = server_config_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_uri));

    httpd_uri_t status_uri = {
        .uri = "/status",  //
        .method = HTTP_GET,
        .handler = server_status_get_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));

    httpd_uri_t wifi_post_uri = {
        .uri = "/wifi",  //
        .method = HTTP_POST,
        .handler = server_wifi_post_handler,
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_post_uri));

    httpd_uri_t common_get_uri = {
        .uri = "/*",  //
        .method = HTTP_GET,
        .handler = server_file_get_handler,
        .user_ctx = server_context
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &common_get_uri));

    return ESP_OK;

err:
    if (server_context)
    {
        free(server_context);
    }
    return ret;
}

esp_err_t server_init(void)
{
    mdns_start();
    netbiosns_init();
    netbiosns_set_name(MDNS_HOST_NAME);
    ESP_RETURN_ON_ERROR(www_fs_init(), TAG, "Failed to initialize WWW FS");
    ESP_RETURN_ON_ERROR(server_start(WWW_FS_BASE_PATH), TAG, "Failed to start server");
    return ESP_OK;
}
