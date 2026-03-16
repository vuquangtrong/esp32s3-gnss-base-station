#include "config.h"

#include <esp_app_desc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <string.h>

#include "util.h"

#define NVS_NAMESPACE "config"

static const char* TAG = "CONFIG";
static nvs_handle_t nvs = 0;

// ordered status list
static char config[CONFIG_MAX][CONFIG_LEN_MAX];
static char config_name[CONFIG_MAX][CONFIG_LEN_MAX / 2] = {
    "hostname",    //
    "version",     //
    "wifi_ssid",   //
    "wifi_pwd",    //
    "ntrip_ip",    //
    "ntrip_port",  //
    "ntrip_user",  //
    "ntrip_pwd",   //
    "ntrip_mnt",   //
    "base_lat",    //
    "base_lon",    //
    "base_alt",    //
};

esp_err_t config_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        ERROR_IF(err != ESP_OK, return err, "Can not erase NVS!");

        err = nvs_flash_init();
        ERROR_IF(err != ESP_OK, return err, "Can not init NVS!");
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ERROR_IF(err != ESP_OK, return err, "Can not open NVS!");

    // erase allocated memory
    memset(config, 0, CONFIG_MAX * CONFIG_LEN_MAX);

    // hostname
    strcpy(config_get(CONFIG_HOSTNAME), "gnss-station");

    // sys version
    const esp_app_desc_t* app_desc = esp_app_get_description();
    strcpy(config_get(CONFIG_VERSION), app_desc->version);

    // load from NVS
    size_t len = 0;
    for (size_t type = CONFIG_NVS_START; type < CONFIG_MAX; type++)
    {
        len = 0;
        err = nvs_get_str(nvs, config_name[type], NULL, &len);
        if (err == ESP_OK && len > 0)
        {
            err = nvs_get_str(nvs, config_name[type], config[type], &len);
            ERROR_IF(err != ESP_OK, continue, "Cannot load config key %s", config_name[type]);
        }

        ESP_LOGI(TAG, "config_init:\r\nkey=%s\r\nval=%s", config_name[type], config[type]);
    }

    return ESP_OK;
}

void config_set(config_t type, const char* value)
{
    memset(config[type], 0, CONFIG_LEN_MAX);
    strncpy(config[type], value, CONFIG_LEN_MAX);

    // save to NVS
    ESP_LOGD(TAG, "config_set:\r\nkey=%s\r\nval=%s", config_name[type], config[type]);
    nvs_set_str(nvs, config_name[type], config[type]);
    nvs_commit(nvs);
}

char* config_get(config_t type)
{
    ESP_LOGD(TAG, "config_get:\r\nkey=%s\r\nval=%s", config_name[type], config[type]);
    return config[type];
}

void config_reset()
{
    esp_err_t err = nvs_flash_erase();
    ERROR_IF(err != ESP_OK, return, "Can not erase NVS!");

    ESP_LOGI(TAG, "Config reset successfully, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
