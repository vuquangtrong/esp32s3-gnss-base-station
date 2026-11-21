#include <stdio.h>

#include "build_info.h"
#include "config.h"
#include "fw_update.h"
#include "sdcard.h"
#include "status.h"
#include "storage.h"

void app_main(void)
{
    printf("GNSS STATION: %s %s %s\n", APP_VERSION, GIT_COMMIT, BUILD_TIME);

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(sdcard_init());

    printf("app=%s, www=%s\n", fw_update_get_active_app(), fw_update_get_active_www());
    printf("%s\n", config_get_all());
    printf("%s\n", status_get_all());
}
