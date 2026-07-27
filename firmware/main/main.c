#include <stdio.h>

#include "config.h"
#include "esp_err.h"

void app_main(void)
{
    printf("===== GNSS STATION =====\n");

    esp_err_t ret = config_init();
    if (ret != ESP_OK)
    {
        printf("Config initialization failed: %s\n", esp_err_to_name(ret));
        return;
    }
}
