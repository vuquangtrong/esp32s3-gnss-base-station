#include "ntrip_server.h"

#include <arpa/inet.h>
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
#include "status.h"
#include "uart.h"

static const char* TAG = "ntrip_server";

static TaskHandle_t g_server_task_handle = NULL;
static volatile bool g_server_running = false;
static int32_t g_listen_fd = -1;
static int32_t g_client_fds[NTRIP_SERVER_MAX_CLIENTS] = {-1, -1, -1, -1};

static void ntrip_server_clear_clients(void)
{
    for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
    {
        if (g_client_fds[i] >= 0)
        {
            close(g_client_fds[i]);
            g_client_fds[i] = -1;
        }
    }
}

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

static void ntrip_server_send_sourcetable(int32_t client_fd)
{
    const char* lat_str = config_get(CFG_BASE_LAT);
    const char* lon_str = config_get(CFG_BASE_LON);
    const char* h_str = config_get(CFG_BASE_HEIGHT);

    double lat = (lat_str != NULL && strlen(lat_str) > 0) ? atof(lat_str) : 0.0;
    double lon = (lon_str != NULL && strlen(lon_str) > 0) ? atof(lon_str) : 0.0;
    double height = (h_str != NULL && strlen(h_str) > 0) ? atof(h_str) : 0.0;

    static char body[512] = {0};
    int32_t body_len = snprintf(
        body, sizeof(body),
        "STR;" NTRIP_SERVER_MOUNTPOINT ";" NTRIP_SERVER_MOUNTPOINT
        ";RTCM3;1005(1),1074(1),1077(1),1084(1),1087(1),1094(1),1097(1),1124(1),1127(1),1230(1);2;GPS+GLO+GAL+BDS;NONE;VN;%.7f;%.7f;0;0;ESP32;none;N;N;0;H=%."
        "4f\r\n"
        "ENDSOURCETABLE\r\n",
        lat, lon, height
    );

    static char header[256] = {0};
    int32_t header_len = snprintf(
        header, sizeof(header),
        "SOURCETABLE 200 OK\r\n"
        "Server: NTRIP ESP32-GNSS-Caster/1.0\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        (long)body_len
    );

    send(client_fd, header, header_len, 0);
    send(client_fd, body, body_len, 0);
    ESP_LOGI(TAG, "Sent sourcetable (lat: %.7f, lon: %.7f, h: %.4f)", lat, lon, height);
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

    // Set receive timeout to read the initial HTTP request line
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static char req_buf[512] = {0};
    int32_t received = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
    if (received <= 0)
    {
        close(client_fd);
        return;
    }
    req_buf[received] = '\0';

    // Parse method and URI: e.g. "GET / HTTP/1.0" or "GET /BASE HTTP/1.1"
    char method[16] = {0};
    char uri[64] = {0};
    if (sscanf(req_buf, "%15s %63s", method, uri) < 2 || strcmp(method, "GET") != 0)
    {
        const char* err_resp = "HTTP/1.0 400 Bad Request\r\n\r\n";
        send(client_fd, err_resp, strlen(err_resp), 0);
        close(client_fd);
        return;
    }

    // Sourcetable requested on "/" or "/sourcetable"
    if (strcmp(uri, "/") == 0 || strcasecmp(uri, "/sourcetable") == 0)
    {
        ntrip_server_send_sourcetable(client_fd);
        close(client_fd);
        return;
    }

    // Mountpoint requested: "/BASE" or "BASE"
    const char* mount_name = (uri[0] == '/') ? &uri[1] : uri;
    if (strcasecmp(mount_name, NTRIP_SERVER_MOUNTPOINT) == 0)
    {
        // Find empty slot for client
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
            const char* busy_resp = "HTTP/1.0 503 Service Unavailable\r\n\r\n";
            send(client_fd, busy_resp, strlen(busy_resp), 0);
            close(client_fd);
            return;
        }

        // Send NTRIP streaming response header
        const char* ok_resp = "ICY 200 OK\r\n\r\n";
        send(client_fd, ok_resp, strlen(ok_resp), 0);

        // Set non-blocking mode for streaming socket
        int32_t flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        g_client_fds[slot] = client_fd;
        ESP_LOGI(TAG, "Client joined slot %ld: %s (/BASE)", (long)slot, client_ip);
        status_set(STT_NTRIP_SERVER_STATUS, CONN_CONNECTED);
        return;
    }

    // Unknown mountpoint
    ESP_LOGW(TAG, "Unknown mountpoint requested: %s", uri);
    const char* not_found_resp = "HTTP/1.0 404 Not Found\r\n\r\n";
    send(client_fd, not_found_resp, strlen(not_found_resp), 0);
    close(client_fd);
}

static void ntrip_server_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NTRIP caster server task started on port %d", NTRIP_SERVER_PORT);

    static uint8_t rx_buf[NTRIP_SERVER_BUFFER_SIZE] = {0};
    static char dummy_rx[64] = {0};

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

        struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};  // 20ms poll interval
        int32_t sel_res = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        if (sel_res > 0)
        {
            // Check for new incoming connection
            if (g_listen_fd >= 0 && FD_ISSET(g_listen_fd, &read_fds))
            {
                ntrip_server_handle_new_connection(g_listen_fd);
            }

            // Check active clients for disconnection / incoming GGA strings
            for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
            {
                if (g_client_fds[i] >= 0 && FD_ISSET(g_client_fds[i], &read_fds))
                {
                    int32_t r = recv(g_client_fds[i], dummy_rx, sizeof(dummy_rx), MSG_DONTWAIT);
                    if (r <= 0)
                    {
                        ESP_LOGI(TAG, "Client slot %ld disconnected", (long)i);
                        close(g_client_fds[i]);
                        g_client_fds[i] = -1;
                    }
                }
            }
        }

        // Broadcast RTCM3 data from UART2 to all connected clients when in Base Fixed mode
        if (status_get_int(STT_GNSS_MODE) == GNSS_BASE)
        {
            size_t buffered_len = 0;
            uart_get_buffered_data_len(UART2_PORT, &buffered_len);

            while (buffered_len > 0 && g_server_running)
            {
                size_t to_read = (buffered_len > sizeof(rx_buf)) ? sizeof(rx_buf) : buffered_len;
                int32_t bytes_read = uart_read_bytes(UART2_PORT, rx_buf, to_read, 0);
                if (bytes_read <= 0)
                {
                    break;
                }

                int32_t total_sent_this_chunk = 0;
                for (int32_t i = 0; i < NTRIP_SERVER_MAX_CLIENTS; i++)
                {
                    if (g_client_fds[i] >= 0)
                    {
                        int32_t sent = send(g_client_fds[i], rx_buf, (size_t)bytes_read, MSG_DONTWAIT | MSG_NOSIGNAL);
                        if (sent < 0)
                        {
                            if (errno != EAGAIN && errno != EWOULDBLOCK)
                            {
                                ESP_LOGW(TAG, "Send error on slot %ld (errno %d), closing", (long)i, errno);
                                close(g_client_fds[i]);
                                g_client_fds[i] = -1;
                            }
                        }
                        else
                        {
                            total_sent_this_chunk += sent;
                        }
                    }
                }

                if (total_sent_this_chunk > 0)
                {
                    status_set(STT_NTRIP_SENT_BYTES, status_get_int(STT_NTRIP_SENT_BYTES) + total_sent_this_chunk);
                }

                uart_get_buffered_data_len(UART2_PORT, &buffered_len);
            }
        }

        // Update server connection status based on active clients
        int32_t active_clients = ntrip_server_get_client_count();
        status_set(STT_NTRIP_SERVER_CLIENT_NUM, active_clients);
        if (active_clients > 0)
        {
            status_set(STT_NTRIP_SERVER_STATUS, CONN_CONNECTED);
        }
        else
        {
            status_set(STT_NTRIP_SERVER_STATUS, CONN_CONNECTING);
        }
    }

    // Cleanup resources
    ntrip_server_clear_clients();
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
    status_set(STT_NTRIP_SERVER_STATUS, CONN_CONNECTING);
    status_set(STT_NTRIP_SERVER_CLIENT_NUM, 0);
    status_set(STT_NTRIP_SENT_BYTES, 0);

    g_server_running = true;

    if (xTaskCreate(ntrip_server_task, "ntrip_caster", 4096, NULL, 5, &g_server_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create NTRIP caster task");
        close(g_listen_fd);
        g_listen_fd = -1;
        g_server_running = false;
        status_set(STT_NTRIP_SERVER_STATUS, CONN_DISCONNECTED);
        status_set(STT_NTRIP_SERVER_CLIENT_NUM, 0);
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

    status_set(STT_NTRIP_SERVER_CLIENT_NUM, 0);
    status_set(STT_NTRIP_SERVER_STATUS, CONN_DISCONNECTED);
    ESP_LOGI(TAG, "NTRIP caster server stopped");
    return ESP_OK;
}
