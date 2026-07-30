#include <stdio.h>

#include "config.h"
#include "server.h"
#include "status.h"
#include "wifi.h"

void app_main(void)
{
    printf("===== GNSS STATION =====\n");

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(server_init());
}
