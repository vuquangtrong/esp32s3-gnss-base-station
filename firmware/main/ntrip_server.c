#include "ntrip_server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "status.h"
#include "uart.h"

static const char* TAG = "ntrip_server";

static TaskHandle_t g_server_task_handle = NULL;
static volatile bool g_server_running = false;
static int32_t g_listen_fd = -1;
static int32_t g_client_fds[NTRIP_SERVER_MAX_CLIENTS] = {[0 ... NTRIP_SERVER_MAX_CLIENTS - 1] = -1};

// Pending output for each client. Sending directly from the caster task makes
// it block on slow clients, which stops UART2 from being drained and corrupts
// the RTCM3 stream for every other client as well.
typedef struct
{
    uint8_t data[NTRIP_SERVER_CLIENT_TX_BUFFER_SIZE];
    size_t offset;
    size_t length;
} ntrip_client_tx_t;

static ntrip_client_tx_t g_client_tx[NTRIP_SERVER_MAX_CLIENTS];

static int32_t ntrip_server_get_client_count(void)
{
    int32_t count = 0;
    for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] >= 0)
        {
            count++;
        }
    }
    return count;
}

// Derive both status values from the client table in one place. Every join and
// leave goes through here so the UI never keeps showing a client that is gone.
static void ntrip_server_update_status(void)
{
    int32_t active_clients = ntrip_server_get_client_count();
    status_set(STT_NTRIP_SERVER_CLIENT_NUM, active_clients);

    connection_status_t server_status = CONN_DISCONNECTED;
    if (g_server_running)
    {
        server_status = (active_clients > 0) ? CONN_CONNECTED : CONN_CONNECTING;
    }
    status_set(STT_NTRIP_SERVER_STATUS, (int)server_status);
}

static void ntrip_server_reset_client_tx(int32_t slot)
{
    if (slot < 0 || slot >= NTRIP_SERVER_MAX_CLIENTS)
    {
        return;
    }

    g_client_tx[slot].offset = 0;
    g_client_tx[slot].length = 0;
}

static void ntrip_server_close_client(int32_t slot)
{
    if (slot < 0 || slot >= NTRIP_SERVER_MAX_CLIENTS || g_client_fds[slot] < 0)
    {
        return;
    }

    close(g_client_fds[slot]);
    g_client_fds[slot] = -1;
    ntrip_server_reset_client_tx(slot);
    ntrip_server_update_status();
}

static void ntrip_server_clear_clients(void)
{
    for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] >= 0)
        {
            close(g_client_fds[i]);
            g_client_fds[i] = -1;
        }
        ntrip_server_reset_client_tx(i);
    }
    ntrip_server_update_status();
}

static void ntrip_server_configure_client_socket(int32_t client_fd)
{
    int32_t opt = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Detect clients that disappear without sending FIN (power loss, network
    // drop). Without keepalive the slot stays occupied until a send fails.
#ifdef TCP_KEEPIDLE
    int32_t keep_idle = NTRIP_SERVER_TCP_KEEPALIVE_IDLE_SEC;
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
#endif
#ifdef TCP_KEEPINTVL
    int32_t keep_interval = NTRIP_SERVER_TCP_KEEPALIVE_INTERVAL_SEC;
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval));
#endif
#ifdef TCP_KEEPCNT
    int32_t keep_count = NTRIP_SERVER_TCP_KEEPALIVE_COUNT;
    setsockopt(client_fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count));
#endif
}

static bool ntrip_server_contains_ignore_case(const char* text, const char* pattern)
{
    if (text == NULL || pattern == NULL)
    {
        return false;
    }

    size_t text_len = strlen(text);
    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0 || pattern_len > text_len)
    {
        return false;
    }

    for (size_t i = 0; i <= text_len - pattern_len; i++)
    {
        bool matched = true;
        for (size_t j = 0; j < pattern_len; j++)
        {
            if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)pattern[j]))
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            return true;
        }
    }
    return false;
}

// Small HTTP responses are sent while the socket is still blocking, so a
// bounded SO_SNDTIMEO is enough to keep this from stalling the caster task.
static bool ntrip_server_send_response(int32_t client_fd, const uint8_t* data, size_t length)
{
    if (client_fd < 0 || data == NULL || length == 0)
    {
        return false;
    }

    size_t sent_total = 0;
    while (sent_total < length)
    {
        int32_t sent = send(client_fd, data + sent_total, length - sent_total, MSG_NOSIGNAL);
        if (sent > 0)
        {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        return false;
    }

    return true;
}

// Buffer RTCM3 bytes for a client instead of sending them inline. A slow
// client must never be allowed to block the task that drains UART2, otherwise
// all clients receive a stream with missing bytes and invalid CRCs.
static bool ntrip_server_queue_client_data(int32_t slot, const uint8_t* data, size_t length)
{
    if (slot < 0 || slot >= NTRIP_SERVER_MAX_CLIENTS || data == NULL || length == 0)
    {
        return false;
    }

    ntrip_client_tx_t* tx = &g_client_tx[slot];

    // Reclaim consumed space at the front before trying to append.
    if (tx->offset > 0 && tx->offset + tx->length + length > sizeof(tx->data))
    {
        if (tx->length > 0)
        {
            memmove(tx->data, tx->data + tx->offset, tx->length);
        }
        tx->offset = 0;
    }

    size_t tail_capacity = sizeof(tx->data) - tx->offset;
    if (length > tail_capacity || tx->length > tail_capacity - length)
    {
        ESP_LOGW(TAG, "Client slot %ld cannot keep up (TX buffer full), closing", (long)slot);
        ntrip_server_close_client(slot);
        return false;
    }

    memcpy(tx->data + tx->offset + tx->length, data, length);
    tx->length += length;
    return true;
}

static void ntrip_server_flush_client(int32_t slot)
{
    if (slot < 0 || slot >= NTRIP_SERVER_MAX_CLIENTS || g_client_fds[slot] < 0)
    {
        return;
    }

    ntrip_client_tx_t* tx = &g_client_tx[slot];
    if (tx->length == 0)
    {
        return;
    }

    int32_t sent = send(g_client_fds[slot], tx->data + tx->offset, tx->length, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0)
    {
        tx->offset += (size_t)sent;
        tx->length -= (size_t)sent;
        if (tx->length == 0)
        {
            tx->offset = 0;
        }
        return;
    }

    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        ESP_LOGW(TAG, "Send error on slot %ld, closing", (long)slot);
        ntrip_server_close_client(slot);
    }
    // EAGAIN/EWOULDBLOCK: keep the data and retry on the next loop iteration.
}

static void ntrip_server_drain_uart2_events(void)
{
    QueueHandle_t queue = uart2_get_event_queue();
    if (queue == NULL)
    {
        return;
    }

    uart_event_t event = {0};
    while (xQueueReceive(queue, &event, 0) == pdTRUE)
    {
        if (event.type == UART_BUFFER_FULL || event.type == UART_FIFO_OVF)
        {
            ESP_LOGW(TAG, "UART2 RX overflow (event %d)", (int)event.type);
        }
    }
}

// Read the whole request header instead of relying on a single recv(), which may
// return only the request line and leave headers (including Ntrip-Version) unread.
static bool ntrip_server_recv_request(int32_t client_fd, char* req_buf, size_t req_buf_size)
{
    if (req_buf == NULL || req_buf_size < 2)
    {
        return false;
    }

    size_t received = 0;
    int32_t elapsed_ms = 0;
    while (received < req_buf_size - 1 && elapsed_ms < NTRIP_SERVER_REQUEST_TIMEOUT_MS)
    {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
        int32_t ready = select(client_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready <= 0)
        {
            elapsed_ms += 20;
            continue;
        }

        int32_t r = recv(client_fd, &req_buf[received], req_buf_size - 1 - received, MSG_DONTWAIT);
        if (r > 0)
        {
            received += (size_t)r;
            req_buf[received] = '\0';
            if (strstr(req_buf, "\r\n\r\n") != NULL || strstr(req_buf, "\n\n") != NULL)
            {
                return true;
            }
            continue;
        }

        if (r == 0)
        {
            return false;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            return false;
        }
    }

    return received > 0;
}

static ntrip_version_t ntrip_server_detect_version(const char* req_buf)
{
#if NTRIP_SERVER_FORCE_VERSION == 1
    return NTRIP_VERSION_1;
#elif NTRIP_SERVER_FORCE_VERSION == 2
    return NTRIP_VERSION_2;
#else
    // NTRIP 1.0 clients (RTKLIB based receivers and DJI CSDK) do not send the
    // Ntrip-Version header and wait for "ICY 200 OK" before reading corrections.
    if (ntrip_server_contains_ignore_case(req_buf, "Ntrip-Version: Ntrip/2.0"))
    {
        return NTRIP_VERSION_2;
    }
    return NTRIP_VERSION_1;
#endif
}

static void ntrip_server_send_sourcetable(int32_t client_fd, ntrip_version_t version)
{
    const char* lat_str = config_get(CFG_BASE_LAT);
    const char* lon_str = config_get(CFG_BASE_LON);
    const char* h_str = config_get(CFG_BASE_HEIGHT);

    double lat = (lat_str != NULL && strlen(lat_str) > 0) ? atof(lat_str) : 0.0;
    double lon = (lon_str != NULL && strlen(lon_str) > 0) ? atof(lon_str) : 0.0;
    double height = (h_str != NULL && strlen(h_str) > 0) ? atof(h_str) : 0.0;

    char body[512] = {0};
    int32_t body_len = snprintf(
        body, sizeof(body),
        "STR;" NTRIP_SERVER_MOUNTPOINT ";" NTRIP_SERVER_MOUNTPOINT
        ";RTCM3;1005(1),1074(1),1077(1),1084(1),1087(1),1094(1),1097(1),1124(1),1127(1),1230(1);2;GPS+GLO+GAL+BDS;NONE;VN;%.7f;%.7f;0;0;ESP32;none;N;N;0;H=%."
        "4f\r\n"
        "ENDSOURCETABLE\r\n",
        lat, lon, height
    );
    if (body_len > (int32_t)sizeof(body) - 1)
    {
        body_len = (int32_t)sizeof(body) - 1;
    }

    char header[320] = {0};
    int32_t header_len = 0;
    if (version == NTRIP_VERSION_2)
    {
        header_len = snprintf(
            header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Server: NTRIP GNSS-Station/1.0\r\n"
            "Ntrip-Version: Ntrip/2.0\r\n"
            "Content-Type: gnss/sourcetable\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n\r\n",
            (long)body_len
        );
    }
    else
    {
        header_len = snprintf(
            header, sizeof(header),
            "SOURCETABLE 200 OK\r\n"
            "Server: NTRIP GNSS-Station/1.0\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %ld\r\n"
            "Connection: close\r\n\r\n",
            (long)body_len
        );
    }
    if (header_len > (int32_t)sizeof(header) - 1)
    {
        header_len = (int32_t)sizeof(header) - 1;
    }

    if (header_len > 0)
    {
        ntrip_server_send_response(client_fd, (const uint8_t*)header, (size_t)header_len);
    }
    if (body_len > 0)
    {
        ntrip_server_send_response(client_fd, (const uint8_t*)body, (size_t)body_len);
    }
    ESP_LOGI(TAG, "Sent sourcetable (lat: %.7f, lon: %.7f, h: %.4f)", lat, lon, height);
}

static void ntrip_server_send_stream_header(int32_t client_fd, ntrip_version_t version)
{
    if (version == NTRIP_VERSION_1)
    {
        const char* icy_resp = "ICY 200 OK\r\n";
        ntrip_server_send_response(client_fd, (const uint8_t*)icy_resp, strlen(icy_resp));
        return;
    }

    const char* ok_resp =
        "HTTP/1.1 200 OK\r\n"
        "Server: NTRIP GNSS-Station/1.0\r\n"
        "Ntrip-Version: Ntrip/2.0\r\n"
        "Content-Type: gnss/data\r\n"
        "Cache-Control: no-store, no-cache, max-age=0\r\n"
        "Pragma: no-cache\r\n\r\n";
    ntrip_server_send_response(client_fd, (const uint8_t*)ok_resp, strlen(ok_resp));
}

static void ntrip_server_handle_new_connection(int32_t listen_fd)
{
    struct sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);
    int32_t client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client_fd < 0)
    {
        return;
    }

    char client_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    ESP_LOGI(TAG, "New connection from %s:%u", client_ip, ntohs(client_addr.sin_port));

    // Bound the blocking request/response phase. The socket is switched to
    // non-blocking only after the stream header has been sent.
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval send_tv = {.tv_sec = 0, .tv_usec = NTRIP_SERVER_RESPONSE_TIMEOUT_MS * 1000};
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));

    char req_buf[NTRIP_SERVER_REQUEST_BUFFER_SIZE] = {0};
    if (!ntrip_server_recv_request(client_fd, req_buf, sizeof(req_buf)))
    {
        close(client_fd);
        return;
    }

    ESP_LOGD(TAG, "Client request: %.128s", req_buf);

    // Parse the request
    char method[16] = {0};
    char uri[64] = {0};
    if (sscanf(req_buf, "%15s %63s", method, uri) < 2 || strcmp(method, "GET") != 0)
    {
        const char* err_resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
        ntrip_server_send_response(client_fd, (const uint8_t*)err_resp, strlen(err_resp));
        close(client_fd);
        return;
    }

    // Determine NTRIP version based on the request headers
    ntrip_version_t version = ntrip_server_detect_version(req_buf);

    // Sourcetable requested on "/" or "/sourcetable"
    if (strcmp(uri, "/") == 0 || strcasecmp(uri, "/sourcetable") == 0)
    {
        ntrip_server_send_sourcetable(client_fd, version);
        close(client_fd);
        return;
    }

    // Mountpoint requested: "/BASE" or "BASE"
    const char* mount_name = (uri[0] == '/') ? &uri[1] : uri;
    if (strcasecmp(mount_name, NTRIP_SERVER_MOUNTPOINT) == 0)
    {
        int32_t slot = -1;
        for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
        {
            if (g_client_fds[i] < 0)
            {
                slot = i;
                break;
            }
        }

        if (slot < 0)
        {
            ESP_LOGW(TAG, "Max clients reached (%d), rejecting %s", NTRIP_SERVER_MAX_CLIENTS, client_ip);
            const char* busy_resp = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            ntrip_server_send_response(client_fd, (const uint8_t*)busy_resp, strlen(busy_resp));
            close(client_fd);
            return;
        }

        // Send the appropriate response header based on the NTRIP version
        ntrip_server_send_stream_header(client_fd, version);
        ntrip_server_configure_client_socket(client_fd);

        int32_t flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        g_client_fds[slot] = client_fd;
        ESP_LOGI(TAG, "Client joined slot %ld: %s (/BASE, NTRIP %d)", (long)slot, client_ip, (version == NTRIP_VERSION_2) ? 2 : 1);
        ntrip_server_update_status();
        return;
    }

    // Unknown mountpoint
    ESP_LOGW(TAG, "Unknown mountpoint requested: %s", uri);
    const char* not_found_resp = "HTTP/1.1 404 Not Found\r\n\r\n";
    ntrip_server_send_response(client_fd, (const uint8_t*)not_found_resp, strlen(not_found_resp));
    close(client_fd);
}

static void ntrip_server_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NTRIP caster server task started on port %d", NTRIP_SERVER_PORT);

    static uint8_t rx_buf[NTRIP_SERVER_BUFFER_SIZE] = {0};
    static char dummy_rx[NTRIP_SERVER_DUMMY_RX_SIZE] = {0};

    while (g_server_running)
    {
        // Setup select fd_set
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int32_t max_fd = g_listen_fd;
        if (g_listen_fd >= 0)
        {
            FD_SET(g_listen_fd, &read_fds);
        }

        for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
        {
            if (g_client_fds[i] >= 0)
            {
                FD_SET(g_client_fds[i], &read_fds);
                if (g_client_fds[i] > max_fd)
                {
                    max_fd = g_client_fds[i];
                }
            }
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = NTRIP_SERVER_SELECT_TIMEOUT_MS * 1000};
        int32_t sel_res = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (sel_res > 0)
        {
            // Check active clients for disconnection / incoming GGA strings before accepting new connections.
            // GGA sentences sent by clients arrive here and are intentionally discarded. Drain the whole
            // readable batch: a single recv() may return GGA bytes and leave the peer's FIN unread, so a
            // one-shot recv() can keep a disconnected client in the slot (and in the UI count) forever.
            for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
            {
                if (g_client_fds[i] < 0 || !FD_ISSET(g_client_fds[i], &read_fds))
                {
                    continue;
                }

                bool disconnected = false;
                for (uint8_t drain = 0; drain < NTRIP_SERVER_RECV_DRAIN_MAX; drain++)
                {
                    int32_t r = recv(g_client_fds[i], dummy_rx, sizeof(dummy_rx), MSG_DONTWAIT);
                    if (r > 0)
                    {
                        continue;
                    }
                    if (r == 0)
                    {
                        disconnected = true;
                        break;
                    }
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        break;
                    }
                    disconnected = true;
                    break;
                }

                // A pending socket error (e.g. RST) is also a disconnect, even if
                // select() only reported the socket as readable.
                if (!disconnected)
                {
                    int32_t socket_error = 0;
                    socklen_t error_len = sizeof(socket_error);
                    if (getsockopt(g_client_fds[i], SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 && socket_error != 0)
                    {
                        disconnected = true;
                    }
                }

                if (disconnected)
                {
                    ESP_LOGI(TAG, "Client slot %ld disconnected", (long)i);
                    ntrip_server_close_client(i);
                }
            }

            // Check for new incoming connection
            if (g_listen_fd >= 0 && FD_ISSET(g_listen_fd, &read_fds))
            {
                ntrip_server_handle_new_connection(g_listen_fd);
            }
        }

        // Broadcast RTCM3 data from UART2 to all connected clients when in Base Fixed mode.
        // Draining UART2 always takes priority: any stall here drops bytes from the
        // RTCM3 stream and breaks its CRCs for every client.
        if (status_get_int(STT_GNSS_MODE) == GNSS_BASE)
        {
            ntrip_server_drain_uart2_events();

            size_t buffered_len = 0;
            int32_t total_queued_this_tick = 0;
            uart_get_buffered_data_len(UART2_PORT, &buffered_len);

            while (buffered_len > 0 && g_server_running)
            {
                size_t to_read = (buffered_len > sizeof(rx_buf)) ? sizeof(rx_buf) : buffered_len;
                int32_t bytes_read = uart_read_bytes(UART2_PORT, rx_buf, to_read, 0);
                if (bytes_read <= 0)
                {
                    break;
                }

                for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
                {
                    if (g_client_fds[i] >= 0 && ntrip_server_queue_client_data(i, rx_buf, (size_t)bytes_read))
                    {
                        ntrip_server_flush_client(i);
                        total_queued_this_tick += bytes_read;
                    }
                }

                uart_get_buffered_data_len(UART2_PORT, &buffered_len);
            }

            // A socket that was not writable last tick may be writable now.
            for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
            {
                ntrip_server_flush_client(i);
            }

            // Publish once per loop instead of once per UART chunk.
            if (total_queued_this_tick > 0)
            {
                status_set(STT_NTRIP_SENT_BYTES, status_get_int(STT_NTRIP_SENT_BYTES) + total_queued_this_tick);
            }
        }

        // Keep the published count in sync even if a slot changed without an
        // explicit event (e.g. a peer dropped between two select() calls).
        ntrip_server_update_status();
    }

    // Cleanup resources
    ntrip_server_clear_clients();
    status_set(STT_NTRIP_SERVER_STATUS, CONN_DISCONNECTED);
    if (g_listen_fd >= 0)
    {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    status_set(STT_NTRIP_SERVER_CLIENT_NUM, 0);
    status_set(STT_NTRIP_SERVER_STATUS, CONN_DISCONNECTED);
    g_server_task_handle = NULL;
    ESP_LOGI(TAG, "NTRIP caster server task stopped");
    vTaskDelete(NULL);
}

esp_err_t ntrip_server_start(void)
{
    if (g_server_running)
    {
        ESP_LOGW(TAG, "NTRIP caster already running");
        return ESP_OK;
    }

    g_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (g_listen_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create listening socket (errno %d)", errno);
        return ESP_FAIL;
    }

    int32_t opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(NTRIP_SERVER_PORT);

    if (bind(g_listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind socket to port %d (errno %d)", NTRIP_SERVER_PORT, errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return ESP_FAIL;
    }

    if (listen(g_listen_fd, NTRIP_SERVER_MAX_CLIENTS) < 0)
    {
        ESP_LOGE(TAG, "Failed to listen on socket (errno %d)", errno);
        close(g_listen_fd);
        g_listen_fd = -1;
        return ESP_FAIL;
    }

    int32_t flags = fcntl(g_listen_fd, F_GETFL, 0);
    fcntl(g_listen_fd, F_SETFL, flags | O_NONBLOCK);

    // Clear UART2 buffer before streaming
    uart_flush_input(UART2_PORT);
    QueueHandle_t uart2_queue = uart2_get_event_queue();
    if (uart2_queue != NULL)
    {
        xQueueReset(uart2_queue);
    }

    ntrip_server_clear_clients();
    status_set(STT_NTRIP_SENT_BYTES, 0);

    g_server_running = true;
    ntrip_server_update_status();

    if (xTaskCreate(ntrip_server_task, "ntrip_caster", NTRIP_SERVER_TASK_STACK_SIZE, NULL, NTRIP_SERVER_TASK_PRIORITY, &g_server_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create NTRIP caster task");
        close(g_listen_fd);
        g_listen_fd = -1;
        g_server_running = false;
        ntrip_server_update_status();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NTRIP caster server started successfully on port %d", NTRIP_SERVER_PORT);
    return ESP_OK;
}

esp_err_t ntrip_server_stop(void)
{
    if (!g_server_running)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping NTRIP caster server...");
    g_server_running = false;

    while (g_server_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ntrip_server_update_status();
    ESP_LOGI(TAG, "NTRIP caster server stopped");
    return ESP_OK;
}
