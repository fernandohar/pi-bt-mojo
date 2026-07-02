/*
 * bt_app_core.h - generic work-queue task to run Bluedroid callbacks off the
 * stack's own callback context. Adapted from the ESP-IDF a2dp_sink example
 * (SPDX-License-Identifier: Unlicense OR CC0-1.0).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define BT_APP_CORE_TAG "BT_APP_CORE"

/* signal for work dispatch */
enum {
    BT_APP_SIG_WORK_DISPATCH = 0x01,
};

/** Handler for a dispatched event (runs in the app task context). */
typedef void (*bt_app_cb_t)(uint16_t event, void *param);

/** Deep-copy callback for parameters that contain pointers. */
typedef void (*bt_app_copy_cb_t)(void *p_dest, void *p_src, int len);

typedef struct {
    uint16_t         sig;
    uint16_t         event;
    bt_app_cb_t      cb;
    void            *param;
} bt_app_msg_t;

bool bt_app_work_dispatch(bt_app_cb_t p_cback, uint16_t event, void *p_params, int param_len, bt_app_copy_cb_t p_copy_cback);
void bt_app_task_start_up(void);
void bt_app_task_shut_down(void);
