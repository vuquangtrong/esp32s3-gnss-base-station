#include "server.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "cJSON.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "gnss.h"
#include "ntrip_client.h"
#include "sdcard.h"
#include "status.h"
#include "storage.h"
#include "uart.h"
#include "wifi.h"

static const char* TAG = "server";

static httpd_handle_t g_server = NULL;
// Pre-allocated static buffers to avoid large stack or frequent heap allocations
static char g_file_buf[SERVER_FILE_BUF_LEN];
static char g_req_buf[SERVER_REQ_BUF_LEN];
static uint8_t g_uart_buf[SERVER_UART_BUF_LEN];

// Flag set when a new WebSocket client connects so the broadcast task immediately sends a status snapshot
static volatile bool g_ws_client_joined = false;

// Cached ETag for files
static etag_cache_entry_t g_etag_cache[ETAG_CACHE_MAX];
static size_t g_etag_cache_count = 0;

static bool server_get_file_etag(const char* filepath, char* etag_out, size_t max_len)
{
    if (filepath == NULL || etag_out == NULL || max_len == 0)
    {
        return false;
    }

    // Check if the ETag is already in the RAM cache
    for (size_t i = 0; i < g_etag_cache_count; i++)
    {
        if (strncmp(g_etag_cache[i].filepath, filepath, SERVER_FILE_PATH_MAX) == 0)
        {
            strlcpy(etag_out, g_etag_cache[i].etag, max_len);
            return true;
        }
    }

    // Cache miss — read CRC from filesystem
    char crc_filepath[SERVER_FILE_PATH_MAX];
    int written = snprintf(crc_filepath, sizeof(crc_filepath), "%s.crc", filepath);
    if (written < 0 || (size_t)written >= sizeof(crc_filepath))
    {
        return false;
    }

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
        crc_buf[strcspn(crc_buf, "\r\n")] = '\0';
        if (crc_buf[0] == '\0')
        {
            return false;
        }

        // HTTP ETags must be wrapped in double quotes
        snprintf(etag_out, max_len, "\"%s\"", crc_buf);

        // Store in cache if space allows
        if (g_etag_cache_count < ETAG_CACHE_MAX)
        {
            strlcpy(g_etag_cache[g_etag_cache_count].filepath, filepath, SERVER_FILE_PATH_MAX);
            strlcpy(g_etag_cache[g_etag_cache_count].etag, etag_out, FILE_ETAG_LENGTH_MAX);
            g_etag_cache_count++;
        }
        return true;
    }

    return false;  // File was empty or failed to read
}

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

static esp_err_t server_handle_sdcard_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, sdcard_list_ubx_files());
}

static esp_err_t server_handle_sdcard_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0 || req->content_len >= sizeof(g_req_buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, g_req_buf, req->content_len);
    if (received <= 0 || (size_t)received != req->content_len)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
        return ESP_FAIL;
    }
    g_req_buf[received] = '\0';

    cJSON* root = cJSON_Parse(g_req_buf);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* cmd_item = cJSON_GetObjectItem(root, "command");
    const cJSON* file_item = cJSON_GetObjectItem(root, "file");
    const char* cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : NULL;
    const char* file = cJSON_IsString(file_item) ? file_item->valuestring : NULL;

    if (cmd == NULL || strcmp(cmd, "delete") != 0 || file == NULL || file[0] == '\0')
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SD card command");
        return ESP_FAIL;
    }

    esp_err_t err = sdcard_file_delete(file);
    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "SDCard POST command failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_handle_wifi_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    char* content = storage_file_read(STORAGE_WIFI_FILE);
    if (content == NULL)
    {
        return httpd_resp_sendstr(req, "{\"data\":[]}");
    }

    esp_err_t ret = httpd_resp_sendstr(req, content);
    free(content);
    return ret;
}

static esp_err_t server_handle_wifi_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0 || req->content_len >= sizeof(g_req_buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, g_req_buf, req->content_len);
    if (received <= 0 || (size_t)received != req->content_len)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
        return ESP_FAIL;
    }
    g_req_buf[received] = '\0';

    cJSON* root = cJSON_Parse(g_req_buf);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char* cmd = cmd_item->valuestring;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(cmd, "connect") == 0 || strcmp(cmd, "save") == 0)
    {
        const cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
        const cJSON* pass_item = cJSON_GetObjectItem(root, "password");
        const char* ssid = cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL;
        const char* password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

        if (ssid == NULL || ssid[0] == '\0')
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID");
            return ESP_FAIL;
        }

        if (strcmp(cmd, "connect") == 0)
        {
            err = wifi_sta_connect(ssid, password);
            if (err == ESP_OK)
            {
                config_set(CFG_WIFI_SSID, ssid);
                config_set(CFG_WIFI_PASSWORD, password);
            }
        }
        else
        {
            cJSON* entry = cJSON_CreateObject();
            if (entry != NULL)
            {
                cJSON_AddStringToObject(entry, "ssid", ssid);
                cJSON_AddStringToObject(entry, "password", password);
                char* entry_str = cJSON_PrintUnformatted(entry);
                if (entry_str != NULL)
                {
                    err = storage_file_write_entry(STORAGE_WIFI_FILE, entry_str);
                    cJSON_free(entry_str);
                }
                cJSON_Delete(entry);
            }
        }
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        const cJSON* idx_item = cJSON_GetObjectItem(root, "index");
        if (cJSON_IsNumber(idx_item) && idx_item->valueint >= 0 && idx_item->valueint <= UINT8_MAX)
        {
            err = storage_file_delete_entry(STORAGE_WIFI_FILE, (uint8_t)idx_item->valueint);
        }
    }

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi POST command failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_handle_ntrip_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    char* content = storage_file_read(STORAGE_NTRIP_FILE);
    if (content == NULL)
    {
        return httpd_resp_sendstr(req, "{\"data\":[]}");
    }

    esp_err_t ret = httpd_resp_sendstr(req, content);
    free(content);
    return ret;
}

static esp_err_t server_handle_ntrip_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0 || req->content_len >= sizeof(g_req_buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, g_req_buf, req->content_len);
    if (received <= 0 || (size_t)received != req->content_len)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
        return ESP_FAIL;
    }
    g_req_buf[received] = '\0';

    cJSON* root = cJSON_Parse(g_req_buf);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char* cmd = cmd_item->valuestring;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(cmd, "query") == 0 || strcmp(cmd, "connect") == 0 || strcmp(cmd, "save") == 0)
    {
        const cJSON* server_item = cJSON_GetObjectItem(root, "server");
        const cJSON* port_item = cJSON_GetObjectItem(root, "port");
        const cJSON* mnt_item = cJSON_GetObjectItem(root, "mntpoint");
        const cJSON* user_item = cJSON_GetObjectItem(root, "username");
        const cJSON* pass_item = cJSON_GetObjectItem(root, "password");

        const char* server = cJSON_IsString(server_item) ? server_item->valuestring : NULL;
        const char* port = cJSON_IsString(port_item) ? port_item->valuestring : NULL;
        const char* mntpoint = cJSON_IsString(mnt_item) ? mnt_item->valuestring : "";
        const char* username = cJSON_IsString(user_item) ? user_item->valuestring : "";
        const char* password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

        char* endptr = NULL;
        unsigned long port_num = (port != NULL && port[0] != '\0') ? strtoul(port, &endptr, 10) : 0;
        if (server == NULL || server[0] == '\0' || port == NULL || port[0] == '\0' || *endptr != '\0' || port_num == 0 || port_num > UINT16_MAX)
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid NTRIP server or port");
            return ESP_FAIL;
        }

        if (strcmp(cmd, "query") == 0)
        {
            config_set(CFG_NTRIP_SERVER, server);
            config_set(CFG_NTRIP_PORT, port);
            config_set(CFG_NTRIP_USERNAME, username);
            config_set(CFG_NTRIP_PASSWORD, password);

            const char* json = ntrip_client_query_mountpoints(server, (uint16_t)port_num, username, password);
            cJSON_Delete(root);
            if (json == NULL)
            {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to query mountpoints");
                return ESP_FAIL;
            }
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, json);
        }
        else if (strcmp(cmd, "connect") == 0)
        {
            if (mntpoint == NULL || mntpoint[0] == '\0')
            {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid NTRIP mountpoint");
                return ESP_FAIL;
            }

            err = ntrip_client_connect(server, (uint16_t)port_num, mntpoint, username, password);
            if (err == ESP_OK)
            {
                config_set(CFG_NTRIP_SERVER, server);
                config_set(CFG_NTRIP_PORT, port);
                config_set(CFG_NTRIP_MNTPOINT, mntpoint);
                config_set(CFG_NTRIP_USERNAME, username);
                config_set(CFG_NTRIP_PASSWORD, password);
            }
        }
        else
        {
            cJSON* entry = cJSON_CreateObject();
            if (entry != NULL)
            {
                cJSON_AddStringToObject(entry, "server", server);
                cJSON_AddStringToObject(entry, "port", port);
                cJSON_AddStringToObject(entry, "mntpoint", mntpoint);
                cJSON_AddStringToObject(entry, "username", username);
                cJSON_AddStringToObject(entry, "password", password);
                char* entry_str = cJSON_PrintUnformatted(entry);
                if (entry_str != NULL)
                {
                    err = storage_file_write_entry(STORAGE_NTRIP_FILE, entry_str);
                    cJSON_free(entry_str);
                }
                cJSON_Delete(entry);
            }
        }
    }
    else if (strcmp(cmd, "disconnect") == 0)
    {
        ntrip_client_disconnect();
        err = ESP_OK;
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        const cJSON* idx_item = cJSON_GetObjectItem(root, "index");
        if (cJSON_IsNumber(idx_item) && idx_item->valueint >= 0 && idx_item->valueint <= UINT8_MAX)
        {
            err = storage_file_delete_entry(STORAGE_NTRIP_FILE, (uint8_t)idx_item->valueint);
        }
    }

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NTRIP POST command failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_handle_base_get(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(req, "application/json");
    char* content = storage_file_read(STORAGE_BASE_FILE);
    if (content == NULL)
    {
        return httpd_resp_sendstr(req, "{\"data\":[]}");
    }

    esp_err_t ret = httpd_resp_sendstr(req, content);
    free(content);
    return ret;
}

static esp_err_t server_handle_base_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0 || req->content_len >= sizeof(g_req_buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, g_req_buf, req->content_len);
    if (received <= 0 || (size_t)received != req->content_len)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
        return ESP_FAIL;
    }
    g_req_buf[received] = '\0';

    cJSON* root = cJSON_Parse(g_req_buf);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* cmd_item = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command");
        return ESP_FAIL;
    }

    const char* cmd = cmd_item->valuestring;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(cmd, "save") == 0)
    {
        const cJSON* name_item = cJSON_GetObjectItem(root, "name");
        const cJSON* lat_item = cJSON_GetObjectItem(root, "lat");
        const cJSON* lon_item = cJSON_GetObjectItem(root, "lon");
        const cJSON* height_item = cJSON_GetObjectItem(root, "height");

        const char* name = cJSON_IsString(name_item) ? name_item->valuestring : NULL;
        const char* lat = cJSON_IsString(lat_item) ? lat_item->valuestring : NULL;
        const char* lon = cJSON_IsString(lon_item) ? lon_item->valuestring : NULL;
        const char* height = cJSON_IsString(height_item) ? height_item->valuestring : NULL;

        if (name == NULL || name[0] == '\0' || lat == NULL || lat[0] == '\0' || lon == NULL || lon[0] == '\0' || height == NULL || height[0] == '\0')
        {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid base position");
            return ESP_FAIL;
        }

        cJSON* entry = cJSON_CreateObject();
        if (entry != NULL)
        {
            cJSON_AddStringToObject(entry, "name", name);
            cJSON_AddStringToObject(entry, "lat", lat);
            cJSON_AddStringToObject(entry, "lon", lon);
            cJSON_AddStringToObject(entry, "height", height);
            char* entry_str = cJSON_PrintUnformatted(entry);
            if (entry_str != NULL)
            {
                err = storage_file_write_entry(STORAGE_BASE_FILE, entry_str);
                cJSON_free(entry_str);
            }
            cJSON_Delete(entry);
        }
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        const cJSON* idx_item = cJSON_GetObjectItem(root, "index");
        if (cJSON_IsNumber(idx_item) && idx_item->valueint >= 0 && idx_item->valueint <= UINT8_MAX)
        {
            err = storage_file_delete_entry(STORAGE_BASE_FILE, (uint8_t)idx_item->valueint);
        }
    }

    cJSON_Delete(root);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Base POST command failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_handle_gnss_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len == 0 || req->content_len >= sizeof(g_req_buf))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, g_req_buf, req->content_len);
    if (received <= 0 || (size_t)received != req->content_len)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
        return ESP_FAIL;
    }
    g_req_buf[received] = '\0';

    cJSON* root = cJSON_Parse(g_req_buf);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON* mode_item = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(mode_item) || mode_item->valuestring == NULL)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing mode");
        return ESP_FAIL;
    }

    const char* mode = mode_item->valuestring;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (strcmp(mode, "rover") == 0)
    {
        err = gnss_set_mode_rover();
    }
    else if (strcmp(mode, "base") == 0)
    {
        const cJSON* lat_item = cJSON_GetObjectItem(root, "lat");
        const cJSON* lon_item = cJSON_GetObjectItem(root, "lon");
        const cJSON* height_item = cJSON_GetObjectItem(root, "height");

        const char* lat_str = cJSON_IsString(lat_item) ? lat_item->valuestring : NULL;
        const char* lon_str = cJSON_IsString(lon_item) ? lon_item->valuestring : NULL;
        const char* height_str = cJSON_IsString(height_item) ? height_item->valuestring : NULL;

        if (lat_str != NULL && lat_str[0] != '\0' && lon_str != NULL && lon_str[0] != '\0' && height_str != NULL && height_str[0] != '\0')
        {
            char* endptr = NULL;
            double lat = strtod(lat_str, &endptr);
            bool lat_ok = (*endptr == '\0');
            double lon = strtod(lon_str, &endptr);
            bool lon_ok = (*endptr == '\0');
            double height = strtod(height_str, &endptr);
            bool height_ok = (*endptr == '\0');

            if (lat_ok && lon_ok && height_ok)
            {
                err = gnss_set_mode_base(lat, lon, height);
                if (err == ESP_OK)
                {
                    config_set(CFG_BASE_LAT, lat_str);
                    config_set(CFG_BASE_LON, lon_str);
                    config_set(CFG_BASE_HEIGHT, height_str);
                }
            }
        }
    }
    else if (strcmp(mode, "ppp") == 0)
    {
        const cJSON* dur_item = cJSON_GetObjectItem(root, "min_dur");
        const cJSON* acc_item = cJSON_GetObjectItem(root, "acc_limit");

        const char* dur_str = cJSON_IsString(dur_item) ? dur_item->valuestring : NULL;
        const char* acc_str = cJSON_IsString(acc_item) ? acc_item->valuestring : NULL;

        if (dur_str != NULL && dur_str[0] != '\0' && acc_str != NULL && acc_str[0] != '\0')
        {
            char* endptr = NULL;
            unsigned long dur = strtoul(dur_str, &endptr, 10);
            bool dur_ok = (*endptr == '\0' && dur > 0 && dur <= UINT32_MAX);
            unsigned long acc = strtoul(acc_str, &endptr, 10);
            bool acc_ok = (*endptr == '\0' && acc > 0 && acc <= UINT32_MAX);

            if (dur_ok && acc_ok)
            {
                err = gnss_set_mode_ppp((uint32_t)dur, (uint32_t)acc);
                if (err == ESP_OK)
                {
                    config_set(CFG_PPP_MIN_DUR, dur_str);
                    config_set(CFG_PPP_ACC_LIMIT, acc_str);
                }
            }
        }
    }

    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid GNSS parameters");
        return ESP_FAIL;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "GNSS POST command failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Command failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_handle_firmware_post(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = fw_update_start();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Firmware update start failed (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start firmware update");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

static esp_err_t server_ws_post_handshake(httpd_req_t* req)
{
    if (req != NULL)
    {
        ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", httpd_req_to_sockfd(req));
    }
    // ESP-IDF consumes the upgrade GET internally and never calls server_handle_ws()
    // for it, so this callback is the reliable place to signal a new client.
    g_ws_client_joined = true;
    return ESP_OK;
}

static esp_err_t server_handle_ws(httpd_req_t* req)
{
    if (req == NULL)
    {
        return ESP_ERR_INVALID_ARG;
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

    // Send frame asynchronously to all active WebSocket connections
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
        // Forward raw GNSS UART bytes (UBX frames) as binary WebSocket frames
        int len = uart_read_bytes(UART1_PORT, g_uart_buf, sizeof(g_uart_buf), pdMS_TO_TICKS(SERVER_UART_TIMEOUT_MS));
        if (len > 0)
        {
            server_ws_broadcast(HTTPD_WS_TYPE_BINARY, g_uart_buf, (size_t)len);
        }

        // Broadcast JSON status whenever system status changes or a new WebSocket client joins
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
    // Strip query string or fragment before mapping URI to file path
    size_t uri_len = strcspn(uri, "?#");

    if (uri_len == 0 || (uri_len == 1 && uri[0] == '/') || (uri_len == sizeof("/index.html") - 1 && strncmp(uri, "/index.html", uri_len) == 0))
    {
        snprintf(filepath, sizeof(filepath), "%s/index.html", WWW_BASE_PATH);
        goto serve_file;  // do not cache index files to force load them
    }
    else
    {
        // Reject path traversal attempts
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

    // get ETag if exists
    char etag[FILE_ETAG_LENGTH_MAX] = {0};
    bool has_etag = server_get_file_etag(filepath, etag, sizeof(etag));

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
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    }

serve_file:
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

    ESP_LOGI(TAG, "Serving static file: %s", filepath);
    httpd_resp_set_type(req, server_content_type(filepath));

    // Stream file in chunks using the pre-allocated static buffer
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
    config.stack_size = 8192;
    // Allow up to 13 client sockets (LWIP max 16 minus 3 internal server sockets)
    // and enable LRU purging so idle connections do not block new clients
    config.max_open_sockets = 13;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
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

    static const httpd_uri_t uri_sdcard_get = {
        .uri = "/api/sdcard",
        .method = HTTP_GET,
        .handler = server_handle_sdcard_get,
    };
    httpd_register_uri_handler(g_server, &uri_sdcard_get);

    static const httpd_uri_t uri_sdcard_post = {
        .uri = "/api/sdcard",
        .method = HTTP_POST,
        .handler = server_handle_sdcard_post,
    };
    httpd_register_uri_handler(g_server, &uri_sdcard_post);

    static const httpd_uri_t uri_wifi_get = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = server_handle_wifi_get,
    };
    httpd_register_uri_handler(g_server, &uri_wifi_get);

    static const httpd_uri_t uri_wifi_post = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = server_handle_wifi_post,
    };
    httpd_register_uri_handler(g_server, &uri_wifi_post);

    static const httpd_uri_t uri_ntrip_get = {
        .uri = "/api/ntrip",
        .method = HTTP_GET,
        .handler = server_handle_ntrip_get,
    };
    httpd_register_uri_handler(g_server, &uri_ntrip_get);

    static const httpd_uri_t uri_ntrip_post = {
        .uri = "/api/ntrip",
        .method = HTTP_POST,
        .handler = server_handle_ntrip_post,
    };
    httpd_register_uri_handler(g_server, &uri_ntrip_post);

    static const httpd_uri_t uri_base_get = {
        .uri = "/api/base",
        .method = HTTP_GET,
        .handler = server_handle_base_get,
    };
    httpd_register_uri_handler(g_server, &uri_base_get);

    static const httpd_uri_t uri_base_post = {
        .uri = "/api/base",
        .method = HTTP_POST,
        .handler = server_handle_base_post,
    };
    httpd_register_uri_handler(g_server, &uri_base_post);

    static const httpd_uri_t uri_gnss_post = {
        .uri = "/api/gnss",
        .method = HTTP_POST,
        .handler = server_handle_gnss_post,
    };
    httpd_register_uri_handler(g_server, &uri_gnss_post);

    static const httpd_uri_t uri_firmware_post = {
        .uri = "/api/firmware",
        .method = HTTP_POST,
        .handler = server_handle_firmware_post,
    };
    httpd_register_uri_handler(g_server, &uri_firmware_post);

    static const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = server_handle_ws,
        .is_websocket = true,
        .ws_post_handshake_cb = server_ws_post_handshake,
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
