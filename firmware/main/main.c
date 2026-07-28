#include <stdio.h>

#include "config.h"
#include "status.h"
#include "wifi.h"

void app_main(void)
{
    printf("===== GNSS STATION =====\n");

    esp_err_t err = config_init();
    if (err != ESP_OK)
    {
        printf("Config initialization failed: %s\n", esp_err_to_name(err));
        return;
    }

    err = wifi_init();
    if (err != ESP_OK)
    {
        printf("WiFi initialization failed: %s\n", esp_err_to_name(err));
        return;
    }
}
