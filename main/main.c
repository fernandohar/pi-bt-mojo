/*
 * main.c - Mojo BT Bridge (ESP32-WROOM) - skeleton
 *
 * Full pipeline (added incrementally):
 *   iPhone --A2DP/AAC--> ESP32 Bluedroid A2DP sink (external codec)
 *          --> AAC decode (esp_audio_codec) / SBC fallback
 *          --> ring buffer + drift compensation
 *          --> software I2S -> S/PDIF (BMC) --> Chord Mojo
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 2
#endif

static const char *TAG = "mojo_bridge";

void app_main(void)
{
    ESP_LOGI(TAG, "=== Mojo BT Bridge (ESP32-WROOM) ===");

    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);

    int level = 0;
    while (1) {
        gpio_set_level(STATUS_LED_GPIO, level);
        level = !level;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
