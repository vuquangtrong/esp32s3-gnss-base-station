#include "server.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "logger.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "sdcard.h"
#include "status.h"
#include "wifi.h"

static const char* TAG = "server";

static httpd_handle_t g_httpd_server_handler = NULL;
static etag_cache_entry_t g_etag_cache[ETAG_CACHE_MAX];
static int g_etag_cache_count = 0;

static void mdns_start(void)
{
    netbiosns_init();
    netbiosns_set_name(MDNS_HOST_NAME);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOST_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE_NAME));

    mdns_txt_item_t serviceTxtData[] = {{"host", MDNS_HOST_NAME ".local"}};
    ESP_ERROR_CHECK(mdns_service_add(MDNS_INSTANCE_NAME, "_http", "_tcp", 80, serviceTxtData, sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));

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
        .base_path = WWW_BASE_PATH,
        .partition_label = WWW_PARTITION_LABEL,
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
        ESP_LOGI(TAG, "Partition size: total: %zu, used: %zu", total, used);
    }
    return ESP_OK;
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
        type = "image/svg+xml";
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
            return true;
        }
    }

    // Cache miss — read CRC from filesystem
    char crc_filepath[SERVER_FILE_PATH_MAX];
    strlcpy(crc_filepath, filepath, sizeof(crc_filepath));
    strlcat(crc_filepath, ".crc", sizeof(crc_filepath));

    int crc_fd = open(crc_filepath, O_RDONLY, 0);
    if (crc_fd == -1)
    {
        return false;  // CRC file not found
    }

    char crc_buf[32];
    ssize_t bytes_read = read(crc_fd, crc_buf, sizeof(crc_buf) - 1);
    close(crc_fd);

    if (bytes_read > 0)
    {
        crc_buf[bytes_read] = '\0';
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

/* Handler for GET files */
static esp_err_t server_get_files_handler(httpd_req_t* req)
{
    char filepath[SERVER_FILE_PATH_MAX];
    char buffer[SERVER_BUFFER_SIZE];

    // reject path traversal attempts
    if (strstr(req->uri, "..") != NULL)
    {
        ESP_LOGE(TAG, "Path traversal rejected: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    strlcpy(filepath, WWW_BASE_PATH, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/')
    {
        strlcat(filepath, "/index.html", sizeof(filepath));
        goto serve_file;  // do not cache index files to force load them
    }
    else
    {
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    // fetch ETag if exists
    char etag[FILE_ETAG_LENGTH_MAX] = {0};
    bool has_etag = get_file_etag(filepath, etag, sizeof(etag));

    // process the ETag and If-None-Match header
    if (has_etag)
    {
        char client_etag[FILE_ETAG_LENGTH_MAX];
        esp_err_t err = httpd_req_get_hdr_value_str(req, "If-None-Match", client_etag, sizeof(client_etag));
        if (err == ESP_OK)
        {
            // if matched, send 304 Not Modified and skip sending the file
            if (strncmp(client_etag, etag, FILE_ETAG_LENGTH_MAX) == 0)
            {
                ESP_LOGI(TAG, "ETag matched for %s. Sending 304.", filepath);
                httpd_resp_set_status(req, "304 Not Modified");
                httpd_resp_send(req, NULL, 0);
                return ESP_OK;
            }
        }
        // if not match, attach the ETag header to the upcoming file response
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    }

serve_file:
    // serve the actual requested file
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    ssize_t read_bytes;
    do
    {
        // read file in chunks into the buffer
        read_bytes = read(fd, buffer, SERVER_BUFFER_SIZE);
        if (read_bytes == -1)
        {
            close(fd);
            ESP_LOGE(TAG, "Failed to read file: %s", filepath);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
            return ESP_FAIL;
        }
        else if (read_bytes > 0)
        {
            // send the buffer contents as HTTP response chunk
            if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK)
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

    // respond with an empty chunk to signal HTTP response completion
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t server_get_sysinfo_handler(httpd_req_t* req)
{
    char query[32] = {0};
    char type[16] = {0};

    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    err = httpd_query_key_value(query, "type", type, sizeof(type));
    if (err != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing type parameter");
        return ESP_FAIL;
    }

    const char* json = NULL;
    if (strcmp(type, "config") == 0)
    {
        json = config_get_all();
    }
    else if (strcmp(type, "status") == 0)
    {
        json = status_get_all();
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid type");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t server_post_wifi_handler(httpd_req_t* req)
{
    char body[256] = {0};
    int32_t received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* command = cJSON_GetObjectItem(root, "command");
    if (command == NULL || !cJSON_IsString(command))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    if (strcmp(command->valuestring, "connect") == 0)
    {
        cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON* password = cJSON_GetObjectItem(root, "password");
        if (ssid == NULL || !cJSON_IsString(ssid) || password == NULL || !cJSON_IsString(password))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid or password");
            return ESP_FAIL;
        }

        config_set(CFG_WIFI_SSID, ssid->valuestring);
        config_set(CFG_WIFI_PASSWORD, password->valuestring);
        wifi_sta_connect(ssid->valuestring, password->valuestring);

        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
    return ESP_FAIL;
}

static esp_err_t server_post_logger_handler(httpd_req_t* req)
{
    char body[128] = {0};
    int32_t received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (root == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* command = cJSON_GetObjectItem(root, "command");
    if (command == NULL || !cJSON_IsString(command))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    if (strcmp(command->valuestring, "start") == 0)
    {
        // Read optional prefix from JSON body
        cJSON* prefix_json = cJSON_GetObjectItem(root, "prefix");
        const char* prefix = (prefix_json != NULL && cJSON_IsString(prefix_json)) ? prefix_json->valuestring : "";

        // Build filename: "prefix_gnss_time.ubx" or "gnss_time.ubx" if no prefix
        const char* gnss_time = status_get_str(STT_GNSS_TIME);
        char filename[96];
        if (prefix[0] != '\0')
        {
            snprintf(filename, sizeof(filename), "%s_%s.ubx", prefix, gnss_time);
        }
        else
        {
            snprintf(filename, sizeof(filename), "%s.ubx", gnss_time);
        }

        esp_err_t err = logger_start(filename);
        cJSON_Delete(root);

        if (err != ESP_OK)
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start logger");
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "stop") == 0)
    {
        cJSON_Delete(root);
        esp_err_t err = logger_stop();

        if (err != ESP_OK)
        {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stop logger");
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "list") == 0)
    {
        cJSON_Delete(root);

        const char* json_str = sdcard_list_ubx_files();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        return ESP_OK;
    }

    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
    return ESP_FAIL;
}

esp_err_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    ESP_ERROR_CHECK(httpd_start(&g_httpd_server_handler, &config));

    httpd_uri_t sysinfo_get_uri = {
        .uri = "/sysinfo",
        .method = HTTP_GET,
        .handler = server_get_sysinfo_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &sysinfo_get_uri));

    httpd_uri_t wifi_post_uri = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = server_post_wifi_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &wifi_post_uri));

    httpd_uri_t logger_post_uri = {
        .uri = "/logger",
        .method = HTTP_POST,
        .handler = server_post_logger_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &logger_post_uri));

    httpd_uri_t common_get_uri = {
        .uri = "/*",  //
        .method = HTTP_GET,
        .handler = server_get_files_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &common_get_uri));

    return ESP_OK;
}

esp_err_t server_init(void)
{
    mdns_start();
    ESP_ERROR_CHECK(www_fs_init());
    ESP_ERROR_CHECK(server_start());
    return ESP_OK;
}
