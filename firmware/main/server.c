#include "server.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "gnss.h"
#include "logger.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "ntrip_client.h"
#include "ntrip_server.h"
#include "sdcard.h"
#include "status.h"
#include "storage.h"
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
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Partition size: total: %zu, used: %zu", total, used);
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

    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static void server_send_json_list(httpd_req_t* req, const char* key, const char* filepath)
{
    char* file_content = storage_read_json_file(filepath);
    cJSON* root = cJSON_CreateObject();
    if (file_content != NULL)
    {
        cJSON* arr = cJSON_Parse(file_content);
        free(file_content);
        if (arr != NULL && cJSON_IsArray(arr))
        {
            cJSON_AddItemToObject(root, key, arr);
        }
        else
        {
            if (arr != NULL)
            {
                cJSON_Delete(arr);
            }
            cJSON_AddArrayToObject(root, key);
        }
    }
    else
    {
        cJSON_AddArrayToObject(root, key);
    }
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (json_str != NULL)
    {
        httpd_resp_sendstr(req, json_str);
        cJSON_free(json_str);
    }
    else
    {
        httpd_resp_sendstr(req, "{}");
    }
}

static esp_err_t server_post_gnss_handler(httpd_req_t* req)
{
    char body[1024] = {0};
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
    if (command != NULL && cJSON_IsString(command))
    {
        if (strcmp(command->valuestring, "save") == 0)
        {
            cJSON* name = cJSON_GetObjectItem(root, "name");
            cJSON* lat = cJSON_GetObjectItem(root, "lat");
            cJSON* lon = cJSON_GetObjectItem(root, "lon");
            cJSON* height = cJSON_GetObjectItem(root, "height");
            if (lat == NULL || !cJSON_IsNumber(lat) || lon == NULL || !cJSON_IsNumber(lon) || height == NULL || !cJSON_IsNumber(height))
            {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing position parameters");
                return ESP_FAIL;
            }

            cJSON* item = cJSON_CreateObject();
            const char* name_str = (name != NULL && cJSON_IsString(name)) ? name->valuestring : "New Point";
            cJSON_AddStringToObject(item, "name", name_str);
            cJSON_AddNumberToObject(item, "lat", lat->valuedouble);
            cJSON_AddNumberToObject(item, "lon", lon->valuedouble);
            cJSON_AddNumberToObject(item, "height", height->valuedouble);

            storage_save_entry(STORAGE_BASE_FILE, item, "name");
            cJSON_Delete(item);
            cJSON_Delete(root);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            return ESP_OK;
        }
        else if (strcmp(command->valuestring, "list") == 0)
        {
            cJSON_Delete(root);
            server_send_json_list(req, "gnss", STORAGE_BASE_FILE);
            return ESP_OK;
        }
        else if (strcmp(command->valuestring, "delete") == 0)
        {
            cJSON* index = cJSON_GetObjectItem(root, "index");
            if (index == NULL || !cJSON_IsNumber(index))
            {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
                return ESP_FAIL;
            }
            storage_delete_entry(STORAGE_BASE_FILE, (int32_t)index->valueint);
            cJSON_Delete(root);

            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            return ESP_OK;
        }
    }

    cJSON* mode = cJSON_GetObjectItem(root, "mode");
    if (mode == NULL || !cJSON_IsString(mode))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode or command");
        return ESP_FAIL;
    }

    const char* mode_name = mode->valuestring;

    if (strcmp(mode_name, "rover") == 0)
    {
        cJSON_Delete(root);

        logger_stop();
        ntrip_server_stop();
        ntrip_client_disconnect_stream();
        gnss_set_mode_rover();
    }
    else if (strcmp(mode_name, "base") == 0)
    {
        cJSON* lat = cJSON_GetObjectItem(root, "lat");
        cJSON* lon = cJSON_GetObjectItem(root, "lon");
        cJSON* height = cJSON_GetObjectItem(root, "height");
        if (lat == NULL || !cJSON_IsNumber(lat) || lon == NULL || !cJSON_IsNumber(lon) || height == NULL || !cJSON_IsNumber(height))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing base position parameters");
            return ESP_FAIL;
        }

        double lat_val = lat->valuedouble;        // deg, 9 decimal places
        double lon_val = lon->valuedouble;        // deg, 9 decimal places
        double height_val = height->valuedouble;  // m, 4 decimal places
        cJSON_Delete(root);

        char value[CFG_VALUE_LENGTH_MAX] = {0};
        snprintf(value, sizeof(value), "%.9f", lat_val);
        config_set(CFG_BASE_LAT, value);
        snprintf(value, sizeof(value), "%.9f", lon_val);
        config_set(CFG_BASE_LON, value);
        snprintf(value, sizeof(value), "%.4f", height_val);
        config_set(CFG_BASE_HEIGHT, value);

        logger_stop();
        ntrip_client_disconnect_stream();
        gnss_base_set_fixed(lat_val, lon_val, height_val);
        gnss_set_mode_base();
        ntrip_server_start();
    }
    else if (strcmp(mode_name, "ppp") == 0)
    {
        cJSON* min_dur = cJSON_GetObjectItem(root, "min_dur");
        cJSON* acc_limit = cJSON_GetObjectItem(root, "acc_limit");
        if (min_dur == NULL || !cJSON_IsNumber(min_dur) || acc_limit == NULL || !cJSON_IsNumber(acc_limit))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing PPP parameters");
            return ESP_FAIL;
        }

        int min_dur_val = (int)min_dur->valuedouble;      // seconds
        int acc_limit_val = (int)acc_limit->valuedouble;  // mm
        cJSON_Delete(root);

        char value[CFG_VALUE_LENGTH_MAX] = {0};
        snprintf(value, sizeof(value), "%d", min_dur_val);
        config_set(CFG_PPP_MIN_DUR, value);
        snprintf(value, sizeof(value), "%d", (int)acc_limit_val);
        config_set(CFG_PPP_ACC_LIMIT, value);

        logger_stop();
        ntrip_server_stop();
        ntrip_client_disconnect_stream();
        gnss_base_set_survey_in(min_dur_val, acc_limit_val * 10 /* at 0.1 scale */);
        gnss_set_mode_ppp();
    }
    else
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown GNSS mode");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t server_post_wifi_handler(httpd_req_t* req)
{
    char body[1024] = {0};
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
    else if (strcmp(command->valuestring, "save") == 0)
    {
        cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON* password = cJSON_GetObjectItem(root, "password");
        if (ssid == NULL || !cJSON_IsString(ssid) || password == NULL || !cJSON_IsString(password))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid or password");
            return ESP_FAIL;
        }

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", ssid->valuestring);
        cJSON_AddStringToObject(item, "password", password->valuestring);

        storage_save_entry(STORAGE_WIFI_FILE, item, "ssid");
        cJSON_Delete(item);
        cJSON_Delete(root);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "list") == 0)
    {
        cJSON_Delete(root);
        server_send_json_list(req, "wifi", STORAGE_WIFI_FILE);
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "delete") == 0)
    {
        cJSON* index = cJSON_GetObjectItem(root, "index");
        if (index == NULL || !cJSON_IsNumber(index))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
            return ESP_FAIL;
        }

        storage_delete_entry(STORAGE_WIFI_FILE, (int32_t)index->valueint);
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
        // Read filename from JSON body
        cJSON* filename_json = cJSON_GetObjectItem(root, "filename");
        if (filename_json == NULL || !cJSON_IsString(filename_json) || filename_json->valuestring[0] == '\0')
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
            return ESP_FAIL;
        }

        const char* filename = filename_json->valuestring;
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

static esp_err_t server_post_ntripclient_handler(httpd_req_t* req)
{
    char body[512] = {0};
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

    if (strcmp(command->valuestring, "get") == 0)
    {
        cJSON_Delete(root);
        const char* json = ntrip_client_get_mountpoints();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "query") == 0)
    {
        cJSON* host = cJSON_GetObjectItem(root, "host");
        cJSON* port = cJSON_GetObjectItem(root, "port");
        cJSON* username = cJSON_GetObjectItem(root, "username");
        cJSON* password = cJSON_GetObjectItem(root, "password");

        if (host == NULL || !cJSON_IsString(host) || port == NULL || !cJSON_IsNumber(port))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing host or port");
            return ESP_FAIL;
        }

        config_set(CFG_NTRIP_SERVER, host->valuestring);
        char port_str[8] = {0};
        snprintf(port_str, sizeof(port_str), "%d", port->valueint);
        config_set(CFG_NTRIP_PORT, port_str);
        if (username != NULL && cJSON_IsString(username))
        {
            config_set(CFG_NTRIP_USERNAME, username->valuestring);
        }
        if (password != NULL && cJSON_IsString(password))
        {
            config_set(CFG_NTRIP_PASSWORD, password->valuestring);
        }

        const char* user_str = (username != NULL && cJSON_IsString(username)) ? username->valuestring : "";
        const char* pass_str = (password != NULL && cJSON_IsString(password)) ? password->valuestring : "";
        ntrip_client_query_mountpoints(host->valuestring, (uint16_t)port->valueint, user_str, pass_str);

        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "connect") == 0)
    {
        cJSON* host = cJSON_GetObjectItem(root, "host");
        cJSON* port = cJSON_GetObjectItem(root, "port");
        cJSON* mountpoint = cJSON_GetObjectItem(root, "mountpoint");
        cJSON* username = cJSON_GetObjectItem(root, "username");
        cJSON* password = cJSON_GetObjectItem(root, "password");

        if (host == NULL || !cJSON_IsString(host) || port == NULL || !cJSON_IsNumber(port) || mountpoint == NULL || !cJSON_IsString(mountpoint))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing host, port, or mountpoint");
            return ESP_FAIL;
        }

        config_set(CFG_NTRIP_SERVER, host->valuestring);
        char port_str[8] = {0};
        snprintf(port_str, sizeof(port_str), "%d", port->valueint);
        config_set(CFG_NTRIP_PORT, port_str);
        config_set(CFG_NTRIP_MOUNTPOINT, mountpoint->valuestring);
        if (username != NULL && cJSON_IsString(username))
        {
            config_set(CFG_NTRIP_USERNAME, username->valuestring);
        }
        if (password != NULL && cJSON_IsString(password))
        {
            config_set(CFG_NTRIP_PASSWORD, password->valuestring);
        }

        const char* user_str = (username != NULL && cJSON_IsString(username)) ? username->valuestring : "";
        const char* pass_str = (password != NULL && cJSON_IsString(password)) ? password->valuestring : "";
        ntrip_client_connect_stream(host->valuestring, (uint16_t)port->valueint, mountpoint->valuestring, user_str, pass_str);

        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "disconnect") == 0)
    {
        cJSON_Delete(root);
        ntrip_client_disconnect_stream();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "save") == 0)
    {
        cJSON* server = cJSON_GetObjectItem(root, "server");
        if (server == NULL)
        {
            server = cJSON_GetObjectItem(root, "host");
        }
        cJSON* port = cJSON_GetObjectItem(root, "port");
        cJSON* mountpoint = cJSON_GetObjectItem(root, "mountpoint");
        cJSON* username = cJSON_GetObjectItem(root, "username");
        cJSON* password = cJSON_GetObjectItem(root, "password");

        if (server == NULL || !cJSON_IsString(server) || port == NULL || !cJSON_IsNumber(port) || mountpoint == NULL || !cJSON_IsString(mountpoint))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing server, port, or mountpoint");
            return ESP_FAIL;
        }

        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "server", server->valuestring);
        cJSON_AddNumberToObject(item, "port", port->valueint);
        cJSON_AddStringToObject(item, "mountpoint", mountpoint->valuestring);
        cJSON_AddStringToObject(item, "username", (username != NULL && cJSON_IsString(username)) ? username->valuestring : "");
        cJSON_AddStringToObject(item, "password", (password != NULL && cJSON_IsString(password)) ? password->valuestring : "");

        storage_save_entry(STORAGE_NTRIP_FILE, item, "mountpoint");
        cJSON_Delete(item);
        cJSON_Delete(root);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "list") == 0)
    {
        cJSON_Delete(root);
        server_send_json_list(req, "ntripclient", STORAGE_NTRIP_FILE);
        return ESP_OK;
    }
    else if (strcmp(command->valuestring, "delete") == 0)
    {
        cJSON* index = cJSON_GetObjectItem(root, "index");
        if (index == NULL || !cJSON_IsNumber(index))
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
            return ESP_FAIL;
        }

        storage_delete_entry(STORAGE_NTRIP_FILE, (int32_t)index->valueint);
        cJSON_Delete(root);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
    return ESP_FAIL;
}

esp_err_t server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
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

    httpd_uri_t ntripclient_post_uri = {
        .uri = "/ntripclient",
        .method = HTTP_POST,
        .handler = server_post_ntripclient_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &ntripclient_post_uri));

    httpd_uri_t gnss_post_uri = {
        .uri = "/gnss",
        .method = HTTP_POST,
        .handler = server_post_gnss_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd_server_handler, &gnss_post_uri));

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
