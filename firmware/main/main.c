#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_4

static void configure_led(void)
{
    gpio_reset_pin(LED_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

static void led_task(void *arg)
{
    uint8_t led_state = 0;
    while (true)
    {
        /* Toggle the LED state */
        led_state = !led_state;
        gpio_set_level(LED_GPIO, led_state);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        printf("LED is %s\n", led_state ? "ON" : "OFF");
    }
}

void app_main(void)
{
    printf("Hello world!\n");
    configure_led();
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
}
