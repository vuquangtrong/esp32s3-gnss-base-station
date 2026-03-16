#include <esp_event.h>

#include "battery.h"
#include "config.h"
#include "ntrip_caster.h"
#include "ntrip_client.h"
#include "ping.h"
#include "status.h"
#include "uart.h"
#include "util.h"
#include "web_app.h"
#include "wifi.h"

static const char* TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GNSS Base Station...");

    // create a default event loop for all tasks
    esp_event_loop_create_default();

    // init NVS and load default settings
    config_init();

    // init status
    status_init();

    // start WiFi AP+STA mode
    wifi_init();

    // start Web App
    web_app_init();

    // start UART ports
    uart_init();

    // start battery monitor
    battery_init();

    // start NTRIP Caster
    ntrip_caster_init();

    // wait for internet
    wait_for_ip();
    ping(config_get(CONFIG_NTRIP_IP));

    // init ntrip client
    ntrip_client_init();
}
