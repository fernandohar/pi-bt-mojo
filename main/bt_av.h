/*
 * bt_av.h - Bluetooth GAP + A2DP sink (external codec) + AVRCP glue.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BT_AV_TAG "bt_av"

/* event dispatched once the Bluetooth stack is up */
enum {
    BT_APP_EVT_STACK_UP = 0,
};

/* status flags for the LED / logging (updated from the BT app task) */
extern volatile bool g_bt_connected;
extern volatile bool g_bt_playing;

/** Stack-up handler: registers GAP/A2DP/AVRCP, SEPs (AAC+SBC) and goes discoverable. */
void bt_av_hdl_stack_evt(uint16_t event, void *p_param);
