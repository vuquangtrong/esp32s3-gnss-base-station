#include "ntrip_caster.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "ntrip_caster";

// Non-blocking TCP listening socket and active client socket table (-1 = empty slot)
static int g_listen_fd = -1;
static int g_client_fds[NTRIP_CASTER_MAX_CLIENTS];

// Task handle and state flags:
// - g_caster_active: requested state (true in BASE mode, false in ROVER/PPP mode)
// - g_caster_running: actual task state, used to synchronize ntrip_caster_stop()
static TaskHandle_t g_caster_task_handle = NULL;
static volatile bool g_caster_active = false;
static volatile bool g_caster_running = false;

// Pre-allocated static buffers for UART2 RTCM3 data and NTRIP HTTP handshakes
static uint8_t g_uart_buf[NTRIP_CASTER_UART_BUF_SIZE];
static char g_req_buf[NTRIP_CASTER_REQ_BUF_SIZE];
static char g_resp_buf[NTRIP_CASTER_RESP_BUF_SIZE];
static char g_str_buf[NTRIP_CASTER_RESP_BUF_SIZE];

static void ntrip_caster_update_status(void)
{
    int32_t count = 0;
    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] >= 0)
        {
            count++;
        }
    }

    // Report connected client count and mark caster status as connected when >= 1 client is streaming
    status_set_int(STT_NTRIP_CONN_COUNT, count);
    status_set_int(STT_NTRIP_SERVER_STATUS, (count > 0) ? CONN_CONNECTED : CONN_DISCONNECTED);
}

static void ntrip_caster_close_all_clients(void)
{
    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] >= 0)
        {
            close(g_client_fds[i]);
            g_client_fds[i] = -1;
        }
    }
    ntrip_caster_update_status();
}

static void ntrip_caster_send_sourcetable(int fd, bool is_v2)
{
    const char* lat = config_get(CFG_BASE_LAT);
    const char* lon = config_get(CFG_BASE_LON);
    const char* height = config_get(CFG_BASE_HEIGHT);

    // Format standard 19-field NTRIP STR record with configured Base LLH and RTCM3 message types enabled in BASE mode
    int body_len = snprintf(
        g_str_buf,
        sizeof(g_str_buf),
        "STR;" NTRIP_CASTER_MOUNTPOINT ";" NTRIP_CASTER_MOUNTPOINT
        ";RTCM 3.3;1005(1),1074(1),1084(1),1094(1),1124(1),1230(1);2;GPS+GLO+GAL+BDS;NONE;VNM;%s;%s;0;0;u-blox;none;B;N;9600;%s\r\n"
        "ENDSOURCETABLE\r\n",
        (lat != NULL) ? lat : "0.0",
        (lon != NULL) ? lon : "0.0",
        (height != NULL) ? height : "0.0");

    if (body_len <= 0 || (size_t)body_len >= sizeof(g_str_buf))
    {
        return;
    }

    // NTRIP v2 uses HTTP/1.1 200 OK with gnss/sourcetable; NTRIP v1 uses SOURCETABLE 200 OK
    int hdr_len = 0;
    if (is_v2)
    {
        hdr_len = snprintf(
            g_resp_buf,
            sizeof(g_resp_buf),
            "HTTP/1.1 200 OK\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "Server: NTRIP Caster\r\n"
            "Content-Type: gnss/sourcetable\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            body_len);
    }
    else
    {
        hdr_len = snprintf(
            g_resp_buf,
            sizeof(g_resp_buf),
            "SOURCETABLE 200 OK\r\n"
            "Server: NTRIP Caster\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            body_len);
    }

    if (hdr_len > 0 && (size_t)hdr_len < sizeof(g_resp_buf))
    {
        send(fd, g_resp_buf, (size_t)hdr_len, 0);
        send(fd, g_str_buf, (size_t)body_len, 0);
    }
}

static void ntrip_caster_accept_clients(void)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int fd = accept(g_listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (fd < 0)
    {
        return;
    }

    // Use a short blocking timeout during initial HTTP request handshake
    struct timeval tv = {
        .tv_sec = NTRIP_CASTER_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (NTRIP_CASTER_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int received = recv(fd, g_req_buf, sizeof(g_req_buf) - 1, 0);
    if (received <= 0)
    {
        close(fd);
        return;
    }
    g_req_buf[received] = '\0';

    // Detect NTRIP v2 via header; default to NTRIP v1 otherwise
    bool is_v2 = (strcasestr(g_req_buf, "Ntrip-Version: Ntrip/2.0") != NULL);
    const char* mount_prefix = "GET /" NTRIP_CASTER_MOUNTPOINT;
    size_t prefix_len = strlen(mount_prefix);

    // Check if the client is requesting the "/BASE" stream or the sourcetable (e.g., "GET /")
    bool is_base_mount = (strncmp(g_req_buf, mount_prefix, prefix_len) == 0) &&
                         (g_req_buf[prefix_len] == ' ' || g_req_buf[prefix_len] == '\r' || g_req_buf[prefix_len] == '\n' || g_req_buf[prefix_len] == '?');

    if (!is_base_mount)
    {
        ESP_LOGI(TAG, "Client requested sourcetable (v%d)", is_v2 ? 2 : 1);
        ntrip_caster_send_sourcetable(fd, is_v2);
        close(fd);
        return;
    }

    // Skip Basic Auth validation on GET /BASE so older devices can connect with or without credentials.
    // Find an available slot in the client table.
    int slot = -1;
    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] < 0)
        {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0)
    {
        ESP_LOGW(TAG, "Max clients reached, rejecting client");
        close(fd);
        return;
    }

    // Send NTRIP v2 (HTTP/1.1 200 OK) or NTRIP v1 (ICY 200 OK) stream start response
    const char* ok_resp = is_v2 ? "HTTP/1.1 200 OK\r\n"
                                  "Ntrip-Version: Ntrip/2.0\r\n"
                                  "Server: NTRIP Caster\r\n"
                                  "Content-Type: gnss/data\r\n"
                                  "Cache-Control: no-store, no-cache, max-age=0\r\n"
                                  "Pragma: no-cache\r\n"
                                  "Connection: close\r\n"
                                  "\r\n"
                                : "ICY 200 OK\r\n\r\n";

    size_t resp_len = strlen(ok_resp);
    if (send(fd, ok_resp, resp_len, 0) != (int)resp_len)
    {
        close(fd);
        return;
    }

    // Switch client socket to non-blocking mode and disable Nagle's algorithm (TCP_NODELAY)
    // so RTCM3 packets are pushed immediately and a slow client never stalls other clients
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    g_client_fds[slot] = fd;
    ntrip_caster_update_status();
    ESP_LOGI(TAG, "Client connected to /" NTRIP_CASTER_MOUNTPOINT " (fd=%d, v%d)", fd, is_v2 ? 2 : 1);
}

static void ntrip_caster_check_clients(void)
{
    bool changed = false;
    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        int fd = g_client_fds[i];
        if (fd < 0)
        {
            continue;
        }

        // Drain any incoming data from the rover (such as periodic NMEA GGA sentences)
        // and detect if the client closed the TCP connection (recv == 0 or fatal error)
        int r = recv(fd, g_req_buf, sizeof(g_req_buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            ESP_LOGI(TAG, "Client disconnected (fd=%d)", fd);
            close(fd);
            g_client_fds[i] = -1;
            changed = true;
        }
    }

    if (changed)
    {
        ntrip_caster_update_status();
    }
}

static void ntrip_caster_broadcast(const uint8_t* data, size_t len)
{
    if (data == NULL || len == 0)
    {
        return;
    }

    bool changed = false;
    int32_t total_sent = 0;

    // Broadcast raw RTCM3 bytes to all connected clients
    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        int fd = g_client_fds[i];
        if (fd < 0)
        {
            continue;
        }

        int sent = send(fd, data, len, 0);
        if (sent < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                ESP_LOGW(TAG, "Send failed, closing client (fd=%d, errno=%d)", fd, errno);
                close(fd);
                g_client_fds[i] = -1;
                changed = true;
            }
        }
        else if (sent > 0)
        {
            total_sent += sent;
        }
    }

    if (total_sent > 0)
    {
        status_set_int(STT_NTRIP_SENT_BYTES, status_get_int(STT_NTRIP_SENT_BYTES) + total_sent);
    }

    if (changed)
    {
        ntrip_caster_update_status();
    }
}

static void ntrip_caster_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NTRIP caster task started on port %d", NTRIP_CASTER_PORT);

    while (1)
    {
        // Idle state (ROVER or PPP mode): close all connected clients and do NOT read UART2
        if (!g_caster_active)
        {
            if (g_caster_running)
            {
                ntrip_caster_close_all_clients();
                g_caster_running = false;
                ESP_LOGI(TAG, "NTRIP caster entered idle state");
            }

            // Drain any connection attempts while idle so the listen backlog does not fill up
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int fd = accept(g_listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (fd >= 0)
            {
                close(fd);
            }

            vTaskDelay(pdMS_TO_TICKS(NTRIP_CASTER_IDLE_DELAY_MS));
            continue;
        }

        // Transitioning from Idle to Active (BASE mode): flush stale UART2 RX bytes and reset counters
        if (!g_caster_running)
        {
            uart_flush_input(UART2_PORT);
            status_set_int(STT_NTRIP_CONN_COUNT, 0);
            status_set_int(STT_NTRIP_SENT_BYTES, 0);
            status_set_int(STT_NTRIP_SERVER_STATUS, CONN_DISCONNECTED);
            g_caster_running = true;
            ESP_LOGI(TAG, "NTRIP caster active in BASE mode");
        }

        // Accept new client connections and clean up disconnected clients
        ntrip_caster_accept_clients();
        ntrip_caster_check_clients();

        // Read RTCM3 messages from UART2 (50 ms timeout to gather full frames) and broadcast immediately
        int len = uart_read_bytes(UART2_PORT, g_uart_buf, sizeof(g_uart_buf), pdMS_TO_TICKS(NTRIP_CASTER_UART_TIMEOUT_MS));
        if (len > 0 && g_caster_active)
        {
            ntrip_caster_broadcast(g_uart_buf, (size_t)len);
        }
    }
}

esp_err_t ntrip_caster_init(void)
{
    if (g_caster_task_handle != NULL)
    {
        return ESP_OK;
    }

    for (size_t i = 0; i < NTRIP_CASTER_MAX_CLIENTS; i++)
    {
        g_client_fds[i] = -1;
    }

    // Create a non-blocking TCP listening socket on port 2101 for all network interfaces (SoftAP + STA)
    g_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (g_listen_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create listen socket: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int flags = fcntl(g_listen_fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(NTRIP_CASTER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        ESP_LOGE(TAG, "Failed to bind socket on port %d: errno %d", NTRIP_CASTER_PORT, errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return ESP_FAIL;
    }

    if (listen(g_listen_fd, NTRIP_CASTER_MAX_CLIENTS) != 0)
    {
        ESP_LOGE(TAG, "Failed to listen on port %d: errno %d", NTRIP_CASTER_PORT, errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(ntrip_caster_task, "ntrip_caster", 4096, NULL, 5, &g_caster_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create NTRIP caster task");
        close(g_listen_fd);
        g_listen_fd = -1;
        g_caster_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ntrip_caster_start(void)
{
    g_caster_active = true;
}

void ntrip_caster_stop(void)
{
    // Signal task to enter idle state and wait until all clients are closed and UART2 reading stops
    g_caster_active = false;
    while (g_caster_running)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
