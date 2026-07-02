/*
 * main.c
 *
 * Wireless iPhone -> Chord Mojo audio bridge, Raspberry Pi Pico 2W.
 *
 *   iPhone --A2DP/SBC--> CYW43439 --> BTstack A2DP sink --> SBC decode
 *          --> drift-compensated buffer --> pico_audio (S/PDIF or I2S) --> Mojo
 *
 * The onboard LED reflects Bluetooth state:
 *   - slow blink : powered on, waiting for a connection
 *   - solid on   : A2DP source connected
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "btstack.h"

#include "a2dp_sink.h"
#include "audio_output.h"

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t led_timer;

static bool bt_up = false;
static bool bt_connected = false;
static int  led_state = 0;

static void led_set(int on) {
    led_state = on ? 1 : 0;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
}

static void led_timer_handler(btstack_timer_source_t *ts) {
    if (bt_connected) {
        led_set(1); // solid when connected
    } else if (bt_up) {
        led_set(1 - led_state); // slow blink while advertising
    } else {
        led_set(0);
    }
    btstack_run_loop_set_timer(ts, 500);
    btstack_run_loop_add_timer(ts);
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    bd_addr_t local_addr;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            bt_up = true;
            gap_local_bd_addr(local_addr);
            printf("[bt] BTstack up on %s, discoverable as \"%s\"\n",
                   bd_addr_to_str(local_addr), BT_DEVICE_NAME);
            printf("[bt] pair from your iPhone (Settings -> Bluetooth) and play audio\n");
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            bt_connected = true;
            printf("[bt] ACL connection established\n");
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            bt_connected = false;
            printf("[bt] ACL disconnected\n");
            break;
        default:
            break;
    }
}

int main(void) {
    stdio_init_all();

    printf("\n=== Mojo BT Bridge (Pico 2W) ===\n");

    // cyw43_arch_init brings up the CYW43439 and (because CYW43_ENABLE_BLUETOOTH=1)
    // the Bluetooth controller + BTstack async-context run loop.
    if (cyw43_arch_init()) {
        printf("[fatal] cyw43_arch_init failed\n");
        return -1;
    }

    printf("[audio] output backend: %s\n", audio_output_backend_name());

    // register the pico_audio-backed sink with BTstack before starting A2DP
    btstack_audio_sink_set_instance(audio_output_get_sink_instance());

    // watch top-level HCI state for status/LED
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // configure A2DP sink + AVRCP + SDP + GAP
    if (a2dp_sink_setup()) {
        printf("[fatal] a2dp_sink_setup failed\n");
        return -1;
    }

    // LED status blinker
    btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
    btstack_run_loop_set_timer(&led_timer, 500);
    btstack_run_loop_add_timer(&led_timer);

    // power on and run
    printf("[bt] starting BTstack...\n");
    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();
    return 0;
}
