#include <stdio.h>

#include "build_info.h"

void app_main(void)
{
    printf("GNSS STATION: %s %s %s\n", APP_VERSION, GIT_COMMIT, BUILD_TIME);
}
