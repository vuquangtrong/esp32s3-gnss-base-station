#include <stdio.h>

#include "build_info.h"
#include "config.h"

void app_main(void)
{
    printf("GNSS STATION: %s %s %s\n", APP_VERSION, GIT_COMMIT, BUILD_TIME);

    ESP_ERROR_CHECK(config_init());
    printf("%s\n", config_get_all());
}
