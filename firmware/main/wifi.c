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

static const char* TAG = "wifi";
static const char* TAG_AP = "wifi.ap";
static const char* TAG_STA = "wifi.sta";

static EventGroupHandle_t g_wifi_event_group = NULL;
static esp_netif_t* g_esp_netif_ap = NULL;
static esp_netif_t* g_esp_netif_sta = NULL;
static wifi_config_t g_wifi_ap_config = {};
static wifi_config_t g_wifi_sta_config = {};
static int g_wifi_sta_retry_num = 0;

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
        if (g_wifi_event_group != NULL)
        {
            xEventGroupSetBits(g_wifi_event_group, WIFI_DISCONNECTED_BIT);
        }

        // Increase retry counter and attempt to reconnect if not exceeded max retries
        g_wifi_sta_retry_num++;
        if (g_wifi_sta_retry_num < WIFI_STA_RETRY_MAX)
        {
            ESP_LOGI(TAG_STA, "Retry to connect to the AP (%d/%d)", g_wifi_sta_retry_num, WIFI_STA_RETRY_MAX);
            status_set(STT_WIFI_STATUS, CONN_CONNECTING);
            status_set(STT_WIFI_IP_ADDR, "");
            vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT / 2));
            esp_wifi_connect();
        }
        else
        {
            ESP_LOGW(TAG_STA, "Failed to connect to the AP after %d attempts", WIFI_STA_RETRY_MAX);
            status_set(STT_WIFI_STATUS, CONN_DISCONNECTED);
            status_set(STT_WIFI_IP_ADDR, "");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG_STA, "Connected to AP, got IP: %s", ip_str);
        status_set(STT_WIFI_STATUS, CONN_CONNECTED);
        status_set(STT_WIFI_IP_ADDR, ip_str);
        // Reset retry counter on successful connection
        g_wifi_sta_retry_num = 0;
    }
}

esp_netif_t* wifi_get_ap_netif(void)
{
    return g_esp_netif_ap;
}

esp_netif_t* wifi_get_sta_netif(void)
{
    return g_esp_netif_sta;
}

esp_netif_t* wifi_init_softap(void)
{
    esp_netif_t* esp_netif_ap = esp_netif_create_default_wifi_ap();

    // get MAC
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    // set soft AP configuration
    snprintf((char*)g_wifi_ap_config.ap.ssid, sizeof(g_wifi_ap_config.ap.ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    g_wifi_ap_config.ap.ssid_len = strlen((char*)g_wifi_ap_config.ap.ssid);
    snprintf((char*)g_wifi_ap_config.ap.password, sizeof(g_wifi_ap_config.ap.password), WIFI_AP_PASSWORD);
    g_wifi_ap_config.ap.channel = 1;
    g_wifi_ap_config.ap.max_connection = 4;
    g_wifi_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    g_wifi_ap_config.ap.pmf_cfg.required = false;

    // set the soft AP configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &g_wifi_ap_config));
    ESP_LOGI(TAG_AP, "Initialized wifi_init_softap SSID:%s password:%s", g_wifi_ap_config.ap.ssid, g_wifi_ap_config.ap.password);

    return esp_netif_ap;
}

esp_netif_t* wifi_init_sta(void)
{
    esp_netif_t* esp_netif_sta = esp_netif_create_default_wifi_sta();

    // set station configuration
    strlcpy((char*)g_wifi_sta_config.sta.ssid, config_get(CFG_WIFI_SSID), sizeof(g_wifi_sta_config.sta.ssid));
    strlcpy((char*)g_wifi_sta_config.sta.password, config_get(CFG_WIFI_PASSWORD), sizeof(g_wifi_sta_config.sta.password));
    g_wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    g_wifi_sta_config.sta.failure_retry_cnt = 10;
    g_wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    g_wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // set the station configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &g_wifi_sta_config));
    ESP_LOGI(TAG_STA, "Initialized wifi_init_sta");

    return esp_netif_sta;
}

esp_err_t wifi_init(void)
{
    // init network interface
    ESP_ERROR_CHECK(esp_netif_init());

    // init event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // register event handlers
    g_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &wifi_event_handler, NULL, NULL));

    // init wifi
    ESP_LOGI(TAG_AP, "Initializing WiFi");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    g_esp_netif_ap = wifi_init_softap();

    ESP_LOGI(TAG_STA, "ESP_WIFI_MODE_STA");
    g_esp_netif_sta = wifi_init_sta();

    // start wifi
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));

    // set default network interface on station interface
    esp_netif_set_default_netif(g_esp_netif_sta);

    ESP_LOGI(TAG, "WiFi initialization completed, default netif set to station interface");
    return ESP_OK;
}

esp_err_t wifi_sta_connect(const char* ssid, const char* password)
{
    // If both SSID and password are provided, update the config
    if (ssid != NULL && password != NULL)
    {
        // Update station configuration with new credentials
        strlcpy((char*)g_wifi_sta_config.sta.ssid, ssid, sizeof(g_wifi_sta_config.sta.ssid));
        strlcpy((char*)g_wifi_sta_config.sta.password, password, sizeof(g_wifi_sta_config.sta.password));

        ESP_LOGI(TAG_STA, "Updated WiFi config SSID:%s password:%s", g_wifi_sta_config.sta.ssid, g_wifi_sta_config.sta.password);

        // disconnect from the current AP before setting new config
        // should prevent auto-reconnect attempts during config change
        g_wifi_sta_retry_num = WIFI_STA_RETRY_MAX;

        // Clear the disconnection event bit before disconnecting
        if (g_wifi_event_group != NULL)
        {
            xEventGroupClearBits(g_wifi_event_group, WIFI_DISCONNECTED_BIT);
        }

        esp_wifi_disconnect();

        // Wait for disconnection event instead of polling
        if (g_wifi_event_group != NULL)
        {
            EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_DISCONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT));
            if ((bits & WIFI_DISCONNECTED_BIT) == 0)
            {
                ESP_LOGW(TAG_STA, "Wait for disconnection timeout, proceeding anyway");
            }
        }

        esp_wifi_set_config(WIFI_IF_STA, &g_wifi_sta_config);
    }
    else
    {
        // Use the last config to reconnect
        ESP_LOGI(TAG_STA, "Use saved WiFi config SSID:%s password:%s", g_wifi_sta_config.sta.ssid, g_wifi_sta_config.sta.password);
    }

    // Reset retry counter to allow fresh connection attempts
    g_wifi_sta_retry_num = 0;
    status_set(STT_WIFI_STATUS, CONN_CONNECTING);
    status_set(STT_WIFI_IP_ADDR, "");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_STA, "Failed to initiate WiFi connection: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
