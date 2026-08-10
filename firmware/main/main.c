#include <stdio.h>

#include "config.h"
#include "logger.h"
#include "parser.h"
#include "server.h"
#include "status.h"
#include "uart.h"
#include "wifi.h"

void app_main(void)
{
    printf("===== GNSS STATION =====\n");

    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(server_init());

    ESP_ERROR_CHECK(parser_init());
    ESP_ERROR_CHECK(parser_task_start());

    ESP_ERROR_CHECK(logger_init());
    ESP_ERROR_CHECK(logger_task_start());

    ESP_ERROR_CHECK(uart_init());
    ESP_ERROR_CHECK(uart1_task_start());
    ESP_ERROR_CHECK(uart2_task_start());
}
