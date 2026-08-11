#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "status.h"

#define WIFI_AP_SSID_PREFIX     "GNSS_BASE_"
#define WIFI_AP_PASSWORD        "12345678"
#define WIFI_STA_RETRY_MAX      10
#define WIFI_DISCONNECTED_BIT   BIT0
#define WIFI_DISCONNECT_TIMEOUT 10000

static const char* TAG = "wifi";
static const char* TAG_AP = "wifi.ap";
static const char* TAG_STA = "wifi.sta";

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t* s_esp_netif_ap = NULL;
static esp_netif_t* s_esp_netif_sta = NULL;
static wifi_config_t s_wifi_ap_config = {};
static wifi_config_t s_wifi_sta_config = {};
static int s_wifi_sta_retry_num = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG_AP, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG_AP, "Station " MACSTR " left, AID=%d, reason:%d", MAC2STR(event->mac), event->aid, event->reason);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT)
    {
        const ip_event_assigned_ip_to_client_t* e = (const ip_event_assigned_ip_to_client_t*)event_data;
        ESP_LOGI(TAG_AP, "Assigned IP to client: " IPSTR ", MAC=" MACSTR ", hostname='%s'", IP2STR(&e->ip), MAC2STR(e->mac), e->hostname);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG_STA, "Station started, connecting to AP");
        wifi_sta_connect(NULL, NULL);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGW(TAG_STA, "Disconnected from AP, reason:%d", event->reason);

        // Signal disconnection event
        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        }

        // Increase retry counter and attempt to reconnect if not exceeded max retries
        s_wifi_sta_retry_num++;
        if (s_wifi_sta_retry_num < WIFI_STA_RETRY_MAX)
        {
            ESP_LOGI(TAG_STA, "Retry to connect to the AP (%d/%d)", s_wifi_sta_retry_num, WIFI_STA_RETRY_MAX);
            status_set(STT_STA_STATUS, WIFI_CONNECTING);
            status_set(STT_STA_IP, "");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGW(TAG_STA, "Failed to connect to the AP after %d attempts", WIFI_STA_RETRY_MAX);
            status_set(STT_STA_STATUS, WIFI_DISCONNECT);
            status_set(STT_STA_IP, "");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG_STA, "Connected to AP, got IP: %s", ip_str);
        status_set(STT_STA_STATUS, WIFI_CONNECTED);
        status_set(STT_STA_IP, ip_str);
        // Reset retry counter on successful connection
        s_wifi_sta_retry_num = 0;
    }
}

esp_netif_t* wifi_get_ap_netif(void)
{
    return s_esp_netif_ap;
}

esp_netif_t* wifi_get_sta_netif(void)
{
    return s_esp_netif_sta;
}

esp_netif_t* wifi_init_softap(void)
{
    esp_netif_t* esp_netif_ap = esp_netif_create_default_wifi_ap();

    // get MAC
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_AP, "Failed to get MAC address: %s", esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG_AP, "MAC address: " MACSTR, MAC2STR(mac));

    // set soft AP configuration
    snprintf((char*)s_wifi_ap_config.ap.ssid, sizeof(s_wifi_ap_config.ap.ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    s_wifi_ap_config.ap.ssid_len = strlen((char*)s_wifi_ap_config.ap.ssid);
    snprintf((char*)s_wifi_ap_config.ap.password, sizeof(s_wifi_ap_config.ap.password), WIFI_AP_PASSWORD);
    s_wifi_ap_config.ap.channel = 1;
    s_wifi_ap_config.ap.max_connection = 4;
    s_wifi_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_wifi_ap_config.ap.pmf_cfg.required = false;

    // set the soft AP configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_wifi_ap_config));
    ESP_LOGI(TAG_AP, "wifi_init_softap initialized. SSID:%s password:%s", s_wifi_ap_config.ap.ssid, s_wifi_ap_config.ap.password);

    return esp_netif_ap;
}

esp_netif_t* wifi_init_sta(void)
{
    esp_netif_t* esp_netif_sta = esp_netif_create_default_wifi_sta();

    // set station configuration
    snprintf((char*)s_wifi_sta_config.sta.ssid, sizeof(s_wifi_sta_config.sta.ssid), "%s", config_get(CFG_WIFI_SSID));
    snprintf((char*)s_wifi_sta_config.sta.password, sizeof(s_wifi_sta_config.sta.password), "%s", config_get(CFG_WIFI_PASSWORD));
    s_wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_wifi_sta_config.sta.failure_retry_cnt = 10;
    s_wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    s_wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // set the station configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_sta_config));
    ESP_LOGI(TAG_STA, "wifi_init_sta initialized.");

    return esp_netif_sta;
}

esp_err_t wifi_init(void)
{
    // init network interface
    ESP_ERROR_CHECK(esp_netif_init());

    // init event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // register event handlers
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &wifi_event_handler, NULL, NULL));

    // init wifi
    ESP_LOGI(TAG_AP, "Initializing WiFi");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    // ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    // ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    // ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    s_esp_netif_ap = wifi_init_softap();

    ESP_LOGI(TAG_STA, "ESP_WIFI_MODE_STA");
    s_esp_netif_sta = wifi_init_sta();

    // start wifi
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));

    // set default network interface on station interface
    esp_netif_set_default_netif(s_esp_netif_sta);

    ESP_LOGI(TAG, "WiFi initialization completed, default netif set to station interface");
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char* ssid, const char* password)
{
    // If both SSID and password are provided, update the config
    if (ssid != NULL && password != NULL)
    {
        // Update station configuration with new credentials
        snprintf((char*)s_wifi_sta_config.sta.ssid, sizeof(s_wifi_sta_config.sta.ssid), "%s", ssid);
        snprintf((char*)s_wifi_sta_config.sta.password, sizeof(s_wifi_sta_config.sta.password), "%s", password);

        ESP_LOGI(TAG_STA, "Updated WiFi config SSID:%s password:%s", s_wifi_sta_config.sta.ssid, s_wifi_sta_config.sta.password);

        // disconnect from the current AP before setting new config
        // should prevent auto-reconnect attempts during config change
        s_wifi_sta_retry_num = WIFI_STA_RETRY_MAX;

        // Clear the disconnection event bit before disconnecting
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT);
        }

        esp_wifi_disconnect();

        // Wait for disconnection event instead of polling
        if (s_wifi_event_group != NULL)
        {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT));
            if ((bits & WIFI_DISCONNECTED_BIT) == 0)
            {
                ESP_LOGW(TAG_STA, "Disconnect timeout, proceeding anyway");
            }
        }

        esp_wifi_set_config(WIFI_IF_STA, &s_wifi_sta_config);
    }
    else
    {
        // Use the last config to reconnect
        ESP_LOGI(TAG_STA, "Using saved WiFi config SSID:%s password:%s", s_wifi_sta_config.sta.ssid, s_wifi_sta_config.sta.password);
    }

    // Reset retry counter to allow fresh connection attempts
    s_wifi_sta_retry_num = 0;
    status_set(STT_STA_STATUS, WIFI_CONNECTING);
    status_set(STT_STA_IP, "");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_STA, "Failed to initiate WiFi connection: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
