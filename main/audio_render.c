/*
 * audio_render.c
 *
 * Pulls encoded A2DP frames from a ring buffer, decodes them to PCM using the
 * esp_audio_codec decoders (AAC primary for the iPhone, SBC fallback), and
 * streams the PCM to the software S/PDIF output.
 *
 * Buffering / drift policy (simple, robust):
 *   - The BT callback pushes encoded frames into a FreeRTOS ring buffer.
 *   - If the ring is full (source faster than our S/PDIF clock) frames are
 *     dropped in audio_render_submit().
 *   - If the ring is empty during playback (source slower) the render task
 *     emits a small block of digital silence to keep the receiver locked.
 *   - The S/PDIF sample rate follows the decoder's reported rate.
 * Finer drift compensation (fractional resampling) is a future refinement.
 */

#include "audio_render.h"
#include "spdif_out.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_audio_dec.h"
#include "esp_aac_dec.h"
#include "esp_sbc_dec.h"

static const char *TAG = "render";

#define ENCODED_RINGBUF_BYTES (24 * 1024)
#define PCM_OUT_BYTES         (8 * 1024)   /* decoded PCM scratch per frame */
#define SILENCE_FRAMES        256          /* silence block on underflow */
#define DEFAULT_SAMPLE_RATE   44100
#define DEFAULT_CHANNELS      2

static RingbufHandle_t s_ring;
static volatile audio_codec_t s_codec = AUDIO_CODEC_NONE;
static volatile uint32_t s_sample_rate = DEFAULT_SAMPLE_RATE;
static volatile uint8_t  s_channels = DEFAULT_CHANNELS;
static volatile bool     s_active;
static volatile bool     s_reopen;

/* current open decoder */
static audio_codec_t s_open_codec = AUDIO_CODEC_NONE;
static void         *s_dec;         /* esp_*_dec handle */

static uint8_t *s_pcm;              /* decoded PCM scratch */
static int16_t *s_stereo;          /* mono->stereo expansion scratch */

static void close_decoder(void)
{
    if (!s_dec) {
        return;
    }
    if (s_open_codec == AUDIO_CODEC_AAC) {
        esp_aac_dec_close(s_dec);
    } else if (s_open_codec == AUDIO_CODEC_SBC) {
        esp_sbc_dec_close(s_dec);
    }
    s_dec = NULL;
    s_open_codec = AUDIO_CODEC_NONE;
}

static bool open_decoder(audio_codec_t codec)
{
    close_decoder();
    esp_audio_err_t err = ESP_AUDIO_ERR_NOT_SUPPORT;

    if (codec == AUDIO_CODEC_AAC) {
        /* A2DP AAC carries raw AAC-LC access units (no ADTS); the format is
         * signalled out-of-band, so provide the negotiated rate/channels. */
        esp_aac_dec_cfg_t cfg = {
            .sample_rate = s_sample_rate,
            .channel = s_channels,
            .bits_per_sample = 16,
            .no_adts_header = true,
            .aac_plus_enable = false,
        };
        err = esp_aac_dec_open(&cfg, sizeof(cfg), &s_dec);
    } else if (codec == AUDIO_CODEC_SBC) {
        esp_sbc_dec_cfg_t cfg = {
            .sbc_mode = ESP_SBC_MODE_STD,
            .ch_num = (s_channels == 1) ? 1 : 2,
            .enable_plc = true,
        };
        err = esp_sbc_dec_open(&cfg, sizeof(cfg), &s_dec);
    }

    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed (codec=%d err=%d)", codec, err);
        s_dec = NULL;
        return false;
    }
    s_open_codec = codec;
    ESP_LOGI(TAG, "opened %s decoder (%" PRIu32 " Hz, %d ch)",
             codec == AUDIO_CODEC_AAC ? "AAC" : "SBC", s_sample_rate, s_channels);
    return true;
}

static esp_audio_err_t decode_one(audio_codec_t codec, esp_audio_dec_in_raw_t *raw,
                                  esp_audio_dec_out_frame_t *out, esp_audio_dec_info_t *info)
{
    if (codec == AUDIO_CODEC_AAC) {
        return esp_aac_dec_decode(s_dec, raw, out, info);
    }
    return esp_sbc_dec_decode(s_dec, raw, out, info);
}

/* Push decoded PCM to S/PDIF, expanding mono to stereo if needed. */
static void output_pcm(const uint8_t *pcm, size_t bytes, uint8_t channels, uint32_t rate)
{
    if (rate && rate != spdif_out_get_rate()) {
        spdif_out_set_rate(rate);
    }
    if (channels == 2) {
        spdif_out_write((const int16_t *)pcm, bytes / 4);
        return;
    }
    /* mono -> duplicate to stereo */
    size_t samples = bytes / 2;
    const int16_t *m = (const int16_t *)pcm;
    size_t off = 0;
    while (off < samples) {
        size_t n = samples - off;
        if (n > PCM_OUT_BYTES / 4) {
            n = PCM_OUT_BYTES / 4;
        }
        for (size_t i = 0; i < n; i++) {
            s_stereo[2 * i] = m[off + i];
            s_stereo[2 * i + 1] = m[off + i];
        }
        spdif_out_write(s_stereo, n);
        off += n;
    }
}

static void render_task(void *arg)
{
    esp_err_t err = spdif_out_init(*(int *)arg, DEFAULT_SAMPLE_RATE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S/PDIF init failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceive(s_ring, &item_size, pdMS_TO_TICKS(15));

        if (item == NULL) {
            /* underflow: keep the S/PDIF link alive during playback gaps */
            if (s_active) {
                spdif_out_write_silence(SILENCE_FRAMES);
            }
            continue;
        }

        if (s_reopen || s_open_codec != s_codec) {
            s_reopen = false;
            open_decoder(s_codec);
        }

        if (s_dec) {
            esp_audio_dec_in_raw_t raw = { .buffer = item, .len = item_size };
            esp_audio_dec_out_frame_t out = { .buffer = s_pcm, .len = PCM_OUT_BYTES };
            esp_audio_dec_info_t info = { 0 };
            while (raw.len > 0) {
                esp_audio_err_t err = decode_one(s_open_codec, &raw, &out, &info);
                if (err == ESP_AUDIO_ERR_OK) {
                    if (out.decoded_size > 0) {
                        output_pcm(out.buffer, out.decoded_size,
                                   info.channel ? info.channel : s_channels,
                                   info.sample_rate ? info.sample_rate : s_sample_rate);
                    }
                    raw.buffer += raw.consumed;
                    raw.len -= raw.consumed;
                } else if (err == ESP_AUDIO_ERR_DATA_LACK) {
                    break; /* need more input for a full frame */
                } else {
                    ESP_LOGW(TAG, "decode error %d", err);
                    break;
                }
            }
        }

        vRingbufferReturnItem(s_ring, item);
    }
}

void audio_render_start(int spdif_gpio)
{
    static int gpio_holder;
    gpio_holder = spdif_gpio;

    s_ring = xRingbufferCreate(ENCODED_RINGBUF_BYTES, RINGBUF_TYPE_NOSPLIT);
    s_pcm = heap_caps_malloc(PCM_OUT_BYTES, MALLOC_CAP_8BIT);
    s_stereo = heap_caps_malloc(PCM_OUT_BYTES, MALLOC_CAP_8BIT);
    if (!s_ring || !s_pcm || !s_stereo) {
        ESP_LOGE(TAG, "alloc failed");
        return;
    }
    /* Pin to the app core so the CPU-bound S/PDIF encoder never starves the
     * Bluetooth stack (which runs on the pro core). */
    BaseType_t core = (configNUM_CORES > 1) ? 1 : tskNO_AFFINITY;
    xTaskCreatePinnedToCore(render_task, "render", 6144, &gpio_holder, 10, NULL, core);
}

void audio_render_set_codec(audio_codec_t codec, uint32_t sample_rate, uint8_t channels)
{
    s_codec = codec;
    s_sample_rate = sample_rate ? sample_rate : DEFAULT_SAMPLE_RATE;
    s_channels = channels ? channels : DEFAULT_CHANNELS;
    s_reopen = true;
    ESP_LOGI(TAG, "codec set: %s %" PRIu32 " Hz %d ch",
             codec == AUDIO_CODEC_AAC ? "AAC" : codec == AUDIO_CODEC_SBC ? "SBC" : "none",
             s_sample_rate, s_channels);
}

void audio_render_set_active(bool active)
{
    s_active = active;
}

void audio_render_submit(const uint8_t *data, size_t len)
{
    if (!s_ring || len == 0) {
        return;
    }
    if (xRingbufferSend(s_ring, data, len, 0) != pdTRUE) {
        /* source faster than sink: drop to bound latency */
        static uint32_t drops;
        if ((++drops % 100) == 0) {
            ESP_LOGW(TAG, "encoded ring full, dropped %" PRIu32 " frames", drops);
        }
    }
}
