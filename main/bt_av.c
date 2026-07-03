/*
 * bt_av.c - GAP + A2DP sink (external codec) + AVRCP for the Mojo BT bridge.
 *
 * Uses the ESP-IDF external-codec A2DP sink path: we register an AAC (M24)
 * stream endpoint (primary, used by the iPhone) and an SBC endpoint (fallback),
 * receive undecoded frames via the audio-data callback, and hand them to
 * audio_render for decoding + S/PDIF output.
 */

#include "bt_av.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

#include "bt_app_core.h"
#include "audio_render.h"

#ifndef BT_DEVICE_NAME
#define BT_DEVICE_NAME "Mojo BT Bridge"
#endif

volatile bool g_bt_connected;
volatile bool g_bt_playing;

/* map AVRCP 0..127 to 0..100 for logging */
static inline int param_scale_vol(uint8_t v) { return v * 100 / 127; }

/* forward declarations */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param);
static void bt_app_a2d_audio_data_cb(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *audio_buf);
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param);

/* ---------------------------------------------------------------------------
 * GAP - pairing (Secure Simple Pairing, just-works) + connection status
 * ------------------------------------------------------------------------- */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(BT_AV_TAG, "authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "SSP confirm request, auto-accepting (val %06" PRIu32 ")", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "SSP passkey: %06" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "SSP passkey requested");
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGI(BT_AV_TAG, "ACL connected, status 0x%x", param->acl_conn_cmpl_stat.stat);
        g_bt_connected = true;
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(BT_AV_TAG, "ACL disconnected, reason 0x%x", param->acl_disconn_cmpl_stat.reason);
        g_bt_connected = false;
        g_bt_playing = false;
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * A2DP sink
 * ------------------------------------------------------------------------- */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_PROF_STATE_EVT:
    case ESP_A2D_SEP_REG_STATE_EVT:
    case ESP_A2D_SNK_PSC_CFG_EVT:
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT:
        bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    default:
        ESP_LOGW(BT_AV_TAG, "unhandled A2DP event: %d", event);
        break;
    }
}

static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)p_param;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "A2DP connection state: %d", a2d->conn_stat.state);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            g_bt_connected = true;
        } else if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            g_bt_connected = false;
            g_bt_playing = false;
            audio_render_set_active(false);
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "A2DP audio state: %d", a2d->audio_stat.state);
        if (a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            g_bt_playing = true;
            audio_render_set_active(true);
        } else {
            g_bt_playing = false;
            audio_render_set_active(false);
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT: {
        esp_a2d_mcc_t *mcc = &a2d->audio_cfg.mcc;
        if (mcc->type == ESP_A2D_MCT_M24) {
            ESP_LOGI(BT_AV_TAG, "codec configured: AAC (assuming 44100 Hz stereo AAC-LC)");
            /* iPhone A2DP AAC is AAC-LC 44.1 kHz stereo; parse CIE for other sources. */
            audio_render_set_codec(AUDIO_CODEC_AAC, 44100, 2);
        } else if (mcc->type == ESP_A2D_MCT_SBC) {
            ESP_LOGI(BT_AV_TAG, "codec configured: SBC");
            audio_render_set_codec(AUDIO_CODEC_SBC, 44100, 2);
        } else {
            ESP_LOGW(BT_AV_TAG, "codec configured: unsupported type 0x%x", mcc->type);
        }
        break;
    }

    case ESP_A2D_SEP_REG_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "SEP register state: seid handled");
        break;

    default:
        break;
    }
}

static void bt_app_a2d_audio_data_cb(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *audio_buf)
{
    audio_render_submit(audio_buf->data, audio_buf->data_len);
    esp_a2d_audio_buff_free(audio_buf);
}

/* ---------------------------------------------------------------------------
 * AVRCP (controller: metadata/notifications, target: absolute volume)
 * ------------------------------------------------------------------------- */
static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP CT connection state: %d", param->conn_stat.connected);
        break;
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP metadata attr 0x%x: %s",
                 param->meta_rsp.attr_id, (char *)param->meta_rsp.attr_text);
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP notify event 0x%x", param->change_ntf.event_id);
        break;
    default:
        break;
    }
}

static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_tg_evt, event, param, sizeof(esp_avrc_tg_cb_param_t), NULL);
        break;
    default:
        break;
    }
}

static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)p_param;
    switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        ESP_LOGI(BT_AV_TAG, "AVRCP absolute volume: %d%% (Mojo controls analog volume)",
                 (int)param_scale_vol(rc->set_abs_vol.volume));
        break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            esp_avrc_rn_param_t rn = { .volume = 0x7f };
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Stack-up: register everything and go discoverable
 * ------------------------------------------------------------------------- */
/*
 * Codec endpoints.
 *
 * IMPORTANT: full AAC A2DP-*sink* stream negotiation only exists on ESP-IDF
 * master (gated by BT_A2DP_CODEC_AAC_ENABLED). On stable releases (e.g. v5.5.1)
 * the external-codec path can pass data but cannot complete the AAC stream open,
 * so if we advertise AAC the iPhone selects it and the stream open fails
 * (BTA_AV_OPEN_EVT::FAILED / BTA_AV_FAIL_STREAM). We therefore advertise
 * SBC only by default; define MOJO_ENABLE_AAC=1 (only on ESP-IDF master with
 * CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y) to also offer AAC.
 */
#ifndef MOJO_ENABLE_AAC
#define MOJO_ENABLE_AAC 0
#endif

/*
 * Registering an AAC endpoint is only useful if the Bluedroid stack was built
 * with AAC negotiation support. Fail loudly rather than produce a binary that
 * advertises AAC but can't open the stream (BTA_AV_OPEN_EVT::FAILED).
 */
#if MOJO_ENABLE_AAC && !defined(CONFIG_BT_A2DP_CODEC_AAC_ENABLED)
#error "MOJO_ENABLE_AAC=1 requires CONFIG_BT_A2DP_CODEC_AAC_ENABLED=y (ESP-IDF v6+). Add the sdkconfig.defaults.aac overlay: -DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.aac\" and re-run set-target."
#endif

static void register_stream_endpoints(void)
{
    uint8_t seid = 0;

#if MOJO_ENABLE_AAC
    /* AAC (M24) endpoint. Uses the proper A2DP AAC codec-capability constants
     * (matching the ESP-IDF v6 a2dp_sink_stream_aac example); v6's real AAC
     * negotiation validates the CIE, so raw 0xff values are rejected and the
     * stream fails to open. */
    esp_a2d_mcc_t aac = { 0 };
    aac.type = ESP_A2D_MCT_M24;
    aac.cie.m24_info.drc = ESP_A2D_M24_CIE_DRC_NS;
    aac.cie.m24_info.obj_type = ESP_A2D_M24_CIE_OBJ_TYPE_2_AAC_LC |
                                ESP_A2D_M24_CIE_OBJ_TYPE_4_AAC_LC |
                                ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC |
                                ESP_A2D_M24_CIE_OBJ_TYPE_4_HE_AAC_V2;
    aac.cie.m24_info.samp_freq1 = ESP_A2D_M24_CIE_SF1_8K | ESP_A2D_M24_CIE_SF1_11K |
                                  ESP_A2D_M24_CIE_SF1_12K | ESP_A2D_M24_CIE_SF1_16K |
                                  ESP_A2D_M24_CIE_SF1_22K | ESP_A2D_M24_CIE_SF1_24K |
                                  ESP_A2D_M24_CIE_SF1_32K | ESP_A2D_M24_CIE_SF1_44K;
    aac.cie.m24_info.samp_freq2 = ESP_A2D_M24_CIE_SF2_48K | ESP_A2D_M24_CIE_SF2_64K |
                                  ESP_A2D_M24_CIE_SF2_88K | ESP_A2D_M24_CIE_SF2_96K;
    aac.cie.m24_info.ch = ESP_A2D_M24_CIE_CH_1 | ESP_A2D_M24_CIE_CH_2;
    aac.cie.m24_info.vbr = ESP_A2D_M24_CIE_VBR_SUPPORT;
    aac.cie.m24_info.br1 = 0x7F & ESP_A2D_M24_CIE_BR1_MSK;
    aac.cie.m24_info.br2 = 0xFF & ESP_A2D_M24_CIE_BR2_MSK;
    aac.cie.m24_info.br3 = 0xFF & ESP_A2D_M24_CIE_BR3_MSK;
    esp_a2d_sink_register_stream_endpoint(seid++, &aac);
    ESP_LOGI(BT_AV_TAG, "registered AAC endpoint");
#endif

    /* SBC endpoint (mandatory, works on all IDF releases) */
    esp_a2d_mcc_t sbc = { 0 };
    sbc.type = ESP_A2D_MCT_SBC;
    sbc.cie.sbc_info.samp_freq = 0xf;
    sbc.cie.sbc_info.ch_mode = 0xf;
    sbc.cie.sbc_info.block_len = 0xf;
    sbc.cie.sbc_info.num_subbands = 0x3;
    sbc.cie.sbc_info.alloc_mthd = 0x3;
    sbc.cie.sbc_info.max_bitpool = 53;
    sbc.cie.sbc_info.min_bitpool = 2;
    esp_a2d_sink_register_stream_endpoint(seid++, &sbc);
    ESP_LOGI(BT_AV_TAG, "registered SBC endpoint");
}

void bt_av_hdl_stack_evt(uint16_t event, void *p_param)
{
    (void)p_param;
    switch (event) {
    case BT_APP_EVT_STACK_UP: {
        esp_bt_gap_set_device_name(BT_DEVICE_NAME);
        esp_bt_gap_register_callback(bt_app_gap_cb);

        /* AVRCP first (coupled to A2DP in Bluedroid) */
        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
        esp_avrc_tg_init();
        esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
        esp_avrc_rn_evt_cap_mask_t evt_set = { 0 };
        esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
        esp_avrc_tg_set_rn_evt_cap(&evt_set);

        /* A2DP sink (external codec) */
        esp_a2d_register_callback(bt_app_a2d_cb);
        esp_a2d_sink_init();
        register_stream_endpoints();
        esp_a2d_sink_register_audio_data_callback(bt_app_a2d_audio_data_cb);

        /* discoverable + connectable */
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        ESP_LOGI(BT_AV_TAG, "discoverable as \"%s\" - pair from the iPhone", BT_DEVICE_NAME);
        break;
    }
    default:
        break;
    }
}
