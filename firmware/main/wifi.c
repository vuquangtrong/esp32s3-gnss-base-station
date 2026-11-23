#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "status.h"

static const char* TAG = "wifi";
static const char* TAG_AP = "wifi.ap";
static const char* TAG_STA = "wifi.sta";

static esp_netif_t* g_esp_netif_ap = NULL;
static esp_netif_t* g_esp_netif_sta = NULL;
static wifi_config_t g_wifi_ap_config = {};
static wifi_config_t g_wifi_sta_config = {};
static uint8_t g_wifi_sta_retry_cnt = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t* event = (const wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG_AP, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t* event = (const wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG_AP, "Station " MACSTR " left, AID=%d, reason:%d", MAC2STR(event->mac), event->aid, event->reason);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT)
    {
        const ip_event_assigned_ip_to_client_t* event = (const ip_event_assigned_ip_to_client_t*)event_data;
        ESP_LOGI(TAG_AP, "Assigned IP to client: " IPSTR ", MAC=" MACSTR ", hostname='%s'", IP2STR(&event->ip), MAC2STR(event->mac), event->hostname);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG_STA, "Station started");
        wifi_sta_connect(NULL, NULL);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        const wifi_event_sta_disconnected_t* event = (const wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG_STA, "Disconnected from AP, reason:%d", event->reason);

        if (event->reason == WIFI_REASON_ASSOC_LEAVE)
        {
            return;
        }

        g_wifi_sta_retry_cnt++;
        if (g_wifi_sta_retry_cnt < WIFI_STA_RETRY_MAX)
        {
            ESP_LOGI(TAG_STA, "Retry to connect to the AP (%u/%u)", g_wifi_sta_retry_cnt, WIFI_STA_RETRY_MAX);
            status_set_int(STT_WIFI_STATUS, CONN_CONNECTING);
            status_set_str(STT_WIFI_IP_ADDR, "");
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGW(TAG_STA, "Failed to connect to the AP after %u attempts", WIFI_STA_RETRY_MAX);
            status_set_int(STT_WIFI_STATUS, CONN_DISCONNECTED);
            status_set_str(STT_WIFI_IP_ADDR, "");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t* event = (const ip_event_got_ip_t*)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG_STA, "Connected to AP, got IP: %s", ip_str);
        status_set_int(STT_WIFI_STATUS, CONN_CONNECTED);
        status_set_str(STT_WIFI_IP_ADDR, ip_str);
        g_wifi_sta_retry_cnt = 0;
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP)
    {
        ESP_LOGW(TAG_STA, "Lost IP address");
        status_set_int(STT_WIFI_STATUS, CONN_CONNECTING);
        status_set_str(STT_WIFI_IP_ADDR, "");
    }
}

static esp_netif_t* wifi_init_softap(void)
{
    esp_netif_t* esp_netif_ap = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    snprintf((char*)g_wifi_ap_config.ap.ssid, sizeof(g_wifi_ap_config.ap.ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    g_wifi_ap_config.ap.ssid_len = strlen((char*)g_wifi_ap_config.ap.ssid);
    config_set(CFG_DEVICE_ID, (char*)g_wifi_ap_config.ap.ssid);
    strlcpy((char*)g_wifi_ap_config.ap.password, WIFI_AP_PASSWORD, sizeof(g_wifi_ap_config.ap.password));
    g_wifi_ap_config.ap.channel = 1;
    g_wifi_ap_config.ap.max_connection = 10;
    g_wifi_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    g_wifi_ap_config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &g_wifi_ap_config));
    ESP_LOGI(TAG_AP, "Initialized SoftAP SSID:%s", g_wifi_ap_config.ap.ssid);

    return esp_netif_ap;
}

static esp_netif_t* wifi_init_sta(void)
{
    esp_netif_t* esp_netif_sta = esp_netif_create_default_wifi_sta();

    strlcpy((char*)g_wifi_sta_config.sta.ssid, config_get(CFG_WIFI_SSID), sizeof(g_wifi_sta_config.sta.ssid));
    strlcpy((char*)g_wifi_sta_config.sta.password, config_get(CFG_WIFI_PASSWORD), sizeof(g_wifi_sta_config.sta.password));
    g_wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    g_wifi_sta_config.sta.failure_retry_cnt = 3;
    g_wifi_sta_config.sta.threshold.authmode = (g_wifi_sta_config.sta.password[0] != '\0') ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    g_wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &g_wifi_sta_config));
    ESP_LOGI(TAG_STA, "Initialized STA");

    return esp_netif_sta;
}

esp_err_t wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &wifi_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Initializing WiFi");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    g_esp_netif_ap = wifi_init_softap();
    g_esp_netif_sta = wifi_init_sta();

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_netif_set_default_netif(g_esp_netif_sta);

    ESP_LOGI(TAG, "WiFi initialization completed");
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char* ssid, const char* password)
{
    if (ssid != NULL)
    {
        strlcpy((char*)g_wifi_sta_config.sta.ssid, ssid, sizeof(g_wifi_sta_config.sta.ssid));
        strlcpy((char*)g_wifi_sta_config.sta.password, (password != NULL) ? password : "", sizeof(g_wifi_sta_config.sta.password));
        g_wifi_sta_config.sta.threshold.authmode = (g_wifi_sta_config.sta.password[0] != '\0') ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

        ESP_LOGI(TAG_STA, "Updated WiFi config SSID:%s", g_wifi_sta_config.sta.ssid);

        esp_wifi_disconnect();
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &g_wifi_sta_config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG_STA, "Failed to set WiFi config: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (g_wifi_sta_config.sta.ssid[0] == '\0')
    {
        ESP_LOGI(TAG_STA, "No WiFi SSID configured, skipping connection");
        status_set_int(STT_WIFI_STATUS, CONN_DISCONNECTED);
        status_set_str(STT_WIFI_IP_ADDR, "");
        return ESP_OK;
    }

    g_wifi_sta_retry_cnt = 0;
    status_set_int(STT_WIFI_STATUS, CONN_CONNECTING);
    status_set_str(STT_WIFI_IP_ADDR, "");

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_STA, "Failed to initiate WiFi connection: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
