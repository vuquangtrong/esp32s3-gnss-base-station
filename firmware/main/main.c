#include <stdio.h>

// #include "esp_timer.h"
#include "battery.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"

void app_main(void)
{
    printf("\nV" VERSION " " GIT_COMMIT " " BUILD_TIME "\n");

    ESP_ERROR_CHECK(config_init());
    printf("\nConfig: %s\n", config_get_all());

    ESP_ERROR_CHECK(battery_init());

    char* stats_buffer = malloc(1024);
    if (stats_buffer == NULL)
    {
        printf("Failed to allocate memory for stats buffer!\n");
        return;
    }

    while (1)
    {
        printf("\nBAT_VOLT: %d\n", status_get_int(STT_BAT_VOLT));

        // Calculate uptime timestamp in seconds
        // int64_t time_us = esp_timer_get_time();
        // uint32_t sec = (uint32_t)(time_us / 1000000ULL);
        // printf("\n[%lu s] SYSTEM STATISTICS REPORT\n", sec);

        // Print Task List (Name, State, Priority, Stack High Water Mark, Task Number)
        // Note: Stack High Water Mark is given in bytes on ESP-IDF FreeRTOS.
        printf("\nName          State   Prio    StackFree TaskID\n");
        vTaskList(stats_buffer);
        printf("%s", stats_buffer);

        // Print CPU Run Time Statistics
        printf("\nTask Name     Abs Time        %% Time\n");
        vTaskGetRunTimeStats(stats_buffer);
        printf("%s", stats_buffer);

        // Print Free Heap Size
        printf("\nHeapFree: %lu B\n", (unsigned long)esp_get_free_heap_size());

        // Delay before refreshing statistics
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Cleanup (though loop runs infinitely)
    free(stats_buffer);
}
