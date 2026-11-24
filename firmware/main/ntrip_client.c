#include "ntrip_client.h"

#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
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
static char g_http_req_buf[NTRIP_REQ_BUF_SIZE];
static char g_http_rx_buf[NTRIP_RX_BUF_SIZE];
static char g_query_line_buf[NTRIP_LINE_BUF_SIZE];
static char g_mntpoints_json[NTRIP_JSON_BUF_SIZE];

static void ntrip_base64_encode(const uint8_t* src, size_t src_len, char* dst, size_t dst_size)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (src == NULL || dst == NULL || dst_size == 0)
    {
        return;
    }

    size_t j = 0;
    for (size_t i = 0; i < src_len && (j + 4) < dst_size; i += 3)
    {
        uint32_t octet_a = src[i];
        uint32_t octet_b = (i + 1 < src_len) ? src[i + 1] : 0;
        uint32_t octet_c = (i + 2 < src_len) ? src[i + 2] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        dst[j++] = b64_table[(triple >> 18) & 0x3F];
        dst[j++] = b64_table[(triple >> 12) & 0x3F];
        dst[j++] = (i + 1 < src_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        dst[j++] = (i + 2 < src_len) ? b64_table[triple & 0x3F] : '=';
    }
    dst[j] = '\0';
}

static int ntrip_open_socket(const char* server, uint16_t port, uint32_t timeout_ms)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo* res = NULL;
    if (getaddrinfo(server, port_str, &hints, &res) != 0 || res == NULL)
    {
        ESP_LOGE(TAG, "DNS lookup failed for %s:%s", server, port_str);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {
        .tv_sec = (time_t)(timeout_ms / 1000),
        .tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000),
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0)
    {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sock;
}

static int ntrip_send_request(const char* server, uint16_t port, const char* path, const char* username, const char* password)
{
    int sock = ntrip_open_socket(server, port, NTRIP_HTTP_TIMEOUT_MS);
    if (sock < 0)
    {
        return -1;
    }

    char auth_hdr[224] = "";
    if (username != NULL && username[0] != '\0')
    {
        char cred[NTRIP_USERNAME_LEN_MAX + NTRIP_PASSWORD_LEN_MAX + 2];
        int cred_len = snprintf(cred, sizeof(cred), "%s:%s", username, (password != NULL) ? password : "");
        if (cred_len > 0 && (size_t)cred_len < sizeof(cred))
        {
            char b64[184];
            ntrip_base64_encode((const uint8_t*)cred, (size_t)cred_len, b64, sizeof(b64));
            snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s\r\n", b64);
        }
    }

    // Send HTTP/1.1 with Ntrip-Version: Ntrip/2.0 so both NTRIP v2 and NTRIP v1 casters accept the request
    int req_len = snprintf(
        g_http_req_buf, sizeof(g_http_req_buf),
        "GET /%s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: NTRIP GNSS/1.0\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        (path != NULL) ? path : "", server, (unsigned)port, auth_hdr
    );

    if (req_len <= 0 || (size_t)req_len >= sizeof(g_http_req_buf) || send(sock, g_http_req_buf, (size_t)req_len, 0) != req_len)
    {
        close(sock);
        return -1;
    }

    return sock;
}

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

    taskENTER_CRITICAL(&g_ntrip_mux);
    g_gga_buf[0] = '\0';
    g_gga_ready = false;
    taskEXIT_CRITICAL(&g_ntrip_mux);

    uart_flush_input(UART2_PORT);
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
                        // g_gga_line_buf[strcspn(g_gga_line_buf, "\r\n")] = '\0';
                        // ESP_LOGI(TAG, "%s", g_gga_line_buf);
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

    int sock = ntrip_send_request(server, port, "", username, password);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to request sourcetable from %s:%u", server, (unsigned)port);
        ntrip_release_caster();
        return NULL;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* data_arr = (root != NULL) ? cJSON_AddArrayToObject(root, "data") : NULL;
    if (data_arr == NULL)
    {
        cJSON_Delete(root);
        close(sock);
        ntrip_release_caster();
        return NULL;
    }

    size_t mnt_count = 0;
    size_t line_len = 0;
    int bytes_read = 0;
    bool first_line = true;
    bool status_ok = false;
    bool done = false;

    while (!done && (bytes_read = recv(sock, g_http_rx_buf, sizeof(g_http_rx_buf), 0)) > 0)
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
                if (first_line)
                {
                    first_line = false;
                    // Support NTRIP v1 ("SOURCETABLE 200 OK") and NTRIP v2 ("HTTP/1.x 200 OK")
                    if (strncasecmp(g_query_line_buf, "SOURCETABLE 200", 15) == 0 ||
                        (strncasecmp(g_query_line_buf, "HTTP/1.", 7) == 0 && strstr(g_query_line_buf, " 200") != NULL))
                    {
                        status_ok = true;
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Caster rejected sourcetable query: %s", g_query_line_buf);
                        done = true;
                        break;
                    }
                }
                else if (strncmp(g_query_line_buf, "ENDSOURCETABLE", 14) == 0)
                {
                    done = true;
                    break;
                }
                else if (mnt_count < NTRIP_MNTPOINTS_MAX && strncmp(g_query_line_buf, "STR;", 4) == 0)
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

    close(sock);

    if (!status_ok)
    {
        cJSON_Delete(root);
        ntrip_release_caster();
        return NULL;
    }

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

    int sock = ntrip_send_request(g_stream_cfg.server, g_stream_cfg.port, g_stream_cfg.mntpoint, g_stream_cfg.username, g_stream_cfg.password);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Failed to connect to caster %s:%u/%s", g_stream_cfg.server, (unsigned)g_stream_cfg.port, g_stream_cfg.mntpoint);
        goto cleanup;
    }

    // Use a 1-second receive timeout during handshake so we can send GGA if the caster waits for position before replying
    struct timeval tv_handshake = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_handshake, sizeof(tv_handshake));

    char tx_gga[NTRIP_NMEA_BUF_SIZE];
    size_t hdr_len = 0;
    size_t payload_offset = 0;
    bool connected = false;
    bool gga_sent = false;
    uint32_t wait_sec = 0;

    while (!g_stream_stop && hdr_len < sizeof(g_http_rx_buf) - 1)
    {
        int r = recv(sock, g_http_rx_buf + hdr_len, sizeof(g_http_rx_buf) - 1 - hdr_len, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            wait_sec++;
            if (ntrip_take_gga(tx_gga, sizeof(tx_gga), !gga_sent))
            {
                if (send(sock, tx_gga, strlen(tx_gga), 0) < 0)
                {
                    ESP_LOGW(TAG, "Failed to send GGA during handshake");
                    goto cleanup;
                }
                gga_sent = true;
                ESP_LOGI(TAG, "Sent GGA: %s", tx_gga);
            }

            if (wait_sec >= (NTRIP_HTTP_TIMEOUT_MS / 1000))
            {
                ESP_LOGE(TAG, "Timed out waiting for caster response header");
                goto cleanup;
            }
            continue;
        }
        if (r <= 0)
        {
            ESP_LOGE(TAG, "Caster closed connection during handshake");
            goto cleanup;
        }

        hdr_len += (size_t)r;
        g_http_rx_buf[hdr_len] = '\0';

        char* line_end = strchr(g_http_rx_buf, '\n');
        if (line_end == NULL)
        {
            continue;
        }

        bool is_icy_200 = (strncasecmp(g_http_rx_buf, "ICY 200", 7) == 0);
        bool is_http_200 = (strncasecmp(g_http_rx_buf, "HTTP/1.", 7) == 0 && strstr(g_http_rx_buf, " 200") != NULL && strstr(g_http_rx_buf, " 200") < line_end);
        bool is_sourcetable = (strncasecmp(g_http_rx_buf, "SOURCETABLE", 11) == 0 || strcasestr(g_http_rx_buf, "gnss/sourcetable") != NULL);

        if (is_sourcetable || (!is_icy_200 && !is_http_200))
        {
            *line_end = '\0';
            if (line_end > g_http_rx_buf && *(line_end - 1) == '\r')
            {
                *(line_end - 1) = '\0';
            }
            ESP_LOGE(TAG, "Caster rejected stream connection: %s", g_http_rx_buf);
            goto cleanup;
        }

        if (is_icy_200)
        {
            // NTRIP v1 "ICY 200 OK\r\n" may be followed by an optional blank "\r\n" or immediately by RTCM bytes
            payload_offset = (size_t)(line_end - g_http_rx_buf + 1);
            if (payload_offset + 2 <= hdr_len && g_http_rx_buf[payload_offset] == '\r' && g_http_rx_buf[payload_offset + 1] == '\n')
            {
                payload_offset += 2;
            }
            else if (payload_offset + 1 <= hdr_len && g_http_rx_buf[payload_offset] == '\n')
            {
                payload_offset += 1;
            }
            connected = true;
            break;
        }

        // Standard HTTP/1.x 200 OK response: wait for end of headers (\r\n\r\n or \n\n)
        char* hdr_end = strstr(g_http_rx_buf, "\r\n\r\n");
        if (hdr_end != NULL)
        {
            payload_offset = (size_t)(hdr_end - g_http_rx_buf + 4);
            connected = true;
            break;
        }
        hdr_end = strstr(g_http_rx_buf, "\n\n");
        if (hdr_end != NULL)
        {
            payload_offset = (size_t)(hdr_end - g_http_rx_buf + 2);
            connected = true;
            break;
        }
    }

    if (!connected)
    {
        ESP_LOGE(TAG, "Caster connection aborted before stream start");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Connected to caster %s:%u/%s", g_stream_cfg.server, (unsigned)g_stream_cfg.port, g_stream_cfg.mntpoint);
    status_set_int(STT_NTRIP_CLIENT_STATUS, CONN_CONNECTED);
    status_set_int(STT_NTRIP_RECEIVED_BYTES, 0);

    // Forward any trailing RTCM bytes already received in the handshake packet
    if (hdr_len > payload_offset)
    {
        size_t init_bytes = hdr_len - payload_offset;
        uart_write_bytes(UART2_PORT, g_http_rx_buf + payload_offset, init_bytes);
        status_set_int(STT_NTRIP_RECEIVED_BYTES, (int32_t)init_bytes);
    }

    // Use a short read timeout for low-latency RTCM forwarding and responsive disconnect
    struct timeval tv = {
        .tv_sec = NTRIP_STREAM_READ_TIMEOUT_MS / 1000,
        .tv_usec = (NTRIP_STREAM_READ_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Send GGA immediately after receiving 200 OK if not already sent during handshake
    if (!gga_sent && ntrip_take_gga(tx_gga, sizeof(tx_gga), true))
    {
        if (send(sock, tx_gga, strlen(tx_gga), 0) < 0)
        {
            ESP_LOGW(TAG, "Failed to send initial GGA to caster");
            goto cleanup;
        }
        ESP_LOGI(TAG, "Sent initial GGA: %s", tx_gga);
    }

    uint32_t idle_ticks = 0;
    uint32_t gga_ticks = 0;

    while (!g_stream_stop)
    {
        gga_ticks++;
        bool force_gga = (gga_ticks >= NTRIP_STREAM_GGA_KEEPALIVE_TICKS);

        // Send whenever a new GGA message arrives from UART2, or periodically as VRS keepalive
        if (ntrip_take_gga(tx_gga, sizeof(tx_gga), force_gga))
        {
            gga_ticks = 0;
            if (send(sock, tx_gga, strlen(tx_gga), 0) < 0)
            {
                ESP_LOGW(TAG, "Failed to send updated GGA to caster");
                break;
            }
            ESP_LOGI(TAG, "Sent GGA: %s", tx_gga);
        }

        int bytes_read = recv(sock, g_http_rx_buf, sizeof(g_http_rx_buf), 0);
        if (bytes_read > 0)
        {
            idle_ticks = 0;
            uart_write_bytes(UART2_PORT, g_http_rx_buf, (size_t)bytes_read);
            int64_t next = (int64_t)status_get_int(STT_NTRIP_RECEIVED_BYTES) + bytes_read;
            status_set_int(STT_NTRIP_RECEIVED_BYTES, (next > INT32_MAX) ? INT32_MAX : (int32_t)next);
        }
        else if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
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
                ESP_LOGW(TAG, "Caster stream read error: %d (errno=%d)", bytes_read, errno);
            }
            break;
        }
    }

cleanup:
    if (sock >= 0)
    {
        close(sock);
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
