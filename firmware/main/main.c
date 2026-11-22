#include <stdio.h>

#include "battery.h"
#include "build_info.h"
#include "config.h"
#include "factory_test.h"
#include "fw_update.h"
#include "sdcard.h"
#include "status.h"
#include "storage.h"
#include "uart.h"
#include "wifi.h"

void app_main(void)
{
    printf("GNSS STATION: %s %s %s\n", APP_VERSION, GIT_COMMIT, BUILD_TIME);

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(sdcard_init());
    ESP_ERROR_CHECK(uart_init());
    ESP_ERROR_CHECK(battery_init());

#if FACTORY_TEST
    ESP_ERROR_CHECK(factory_test_uart());
    return;
#endif

    ESP_ERROR_CHECK(wifi_init());

    printf("app=%s, www=%s\n", fw_update_get_active_app(), fw_update_get_active_www());
    printf("%s\n", config_get_all());
    printf("%s\n", status_get_all());
}
