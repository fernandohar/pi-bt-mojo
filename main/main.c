/*
 * main.c - Mojo BT Bridge (ESP32-WROOM)
 *
 *   iPhone --A2DP/AAC--> ESP32 Bluedroid A2DP sink (external codec)
 *          --> AAC decode (esp_audio_codec) / SBC fallback
 *          --> ring buffer + drift policy
 *          --> software I2S -> S/PDIF (BMC) --> Chord Mojo
 *
 * Status LED: slow blink = discoverable/idle, solid = connected/playing.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"

#include "bt_app_core.h"
#include "bt_av.h"
#include "audio_render.h"

#ifndef STATUS_LED_GPIO
#define STATUS_LED_GPIO 2
#endif

#ifndef SPDIF_GPIO
#define SPDIF_GPIO 27
#endif

static const char *TAG = "mojo_bridge";

static void led_task(void *arg)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = 1ULL << STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);

    int level = 0;
    for (;;) {
        if (g_bt_connected) {
            gpio_set_level(STATUS_LED_GPIO, 1); /* solid when connected */
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            level = !level;
            gpio_set_level(STATUS_LED_GPIO, level); /* blink while discoverable */
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Mojo BT Bridge (ESP32-WROOM) ===");

    /* NVS (used for BT link keys + PHY calibration) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* start audio pipeline (S/PDIF output + decoder task) before BT */
    audio_render_start(SPDIF_GPIO);

    /* classic-only: release BLE controller memory */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Secure Simple Pairing: display+yes/no, auto-confirmed in the GAP handler */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    /* legacy pairing fallback pin */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    const uint8_t *bda = esp_bt_dev_get_address();
    if (bda) {
        ESP_LOGI(TAG, "own address: %02x:%02x:%02x:%02x:%02x:%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    }

    bt_app_task_start_up();
    bt_app_work_dispatch(bt_av_hdl_stack_evt, BT_APP_EVT_STACK_UP, NULL, 0, NULL);

    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
}
