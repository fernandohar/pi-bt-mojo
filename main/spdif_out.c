/*
 * spdif_out.c - software S/PDIF (AES3/IEC 60958) output via ESP32 I2S.
 *
 * Clean-room biphase-mark encoder built from the public S/PDIF frame spec:
 *   - Each subframe is 32 time slots: 4-slot preamble + 24-bit sample (slots
 *     4..27, LSB first) + Validity(28) + User(29) + Channel(30) + Parity(31).
 *   - Every non-preamble slot is biphase-mark coded to 2 UI (a transition at
 *     the start of every UI-pair, plus a mid transition for a '1').
 *   - Preambles B/M/W are fixed 8-UI sync patterns (BMC violations), inverted
 *     when the previous UI was high.
 *   - A block is 192 frames; the first left subframe of a block uses preamble
 *     B, other left subframes use M, right subframes use W.
 *
 * The 128 UI per stereo frame are streamed as 4x 32-bit I2S words, so the I2S
 * peripheral runs at 2x the audio sample rate with 32-bit stereo slots
 * (bit clock = 128 * Fs = the S/PDIF UI rate). APLL is used for low jitter.
 */

#include "spdif_out.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "spdif";

/*
 * Some ESP32 revisions swap the two 16-bit halves of each 32-bit I2S word.
 * If a scope/DAC shows the bitstream is half-word swapped, set this to 1.
 */
#ifndef SPDIF_SWAP_WORDS
#define SPDIF_SWAP_WORDS 0
#endif

#define SPDIF_WORDS_PER_FRAME 4     /* 128 UI / 32 bits */
#define SPDIF_CHUNK_FRAMES    192   /* one S/PDIF block per I2S write */

static i2s_chan_handle_t s_tx;
static int      s_gpio = -1;
static uint32_t s_fs;               /* audio sample rate */
static uint8_t  s_prev;             /* level of last emitted UI */
static uint16_t s_block_frame;      /* 0..191 within the S/PDIF block */
static uint32_t s_chunk[SPDIF_CHUNK_FRAMES * SPDIF_WORDS_PER_FRAME];

/* Preamble UI patterns (assume previous UI == 0; inverted otherwise). */
static const uint8_t PREAMBLE_B[8] = {1, 1, 1, 0, 1, 0, 0, 0}; /* block start, left */
static const uint8_t PREAMBLE_M[8] = {1, 1, 1, 0, 0, 0, 1, 0}; /* left */
static const uint8_t PREAMBLE_W[8] = {1, 1, 1, 0, 0, 1, 0, 0}; /* right */

/* Build one 64-UI subframe into out[2] (MSB-first: UI0 -> bit31 of out[0]). */
static void build_subframe(uint16_t sample, const uint8_t *preamble, uint32_t out[2])
{
    uint8_t ui[64];
    int n = 0;

    /* preamble: fixed pattern, inverted if the previous UI was high */
    for (int i = 0; i < 8; i++) {
        uint8_t lvl = preamble[i];
        if (s_prev) {
            lvl = !lvl;
        }
        ui[n++] = lvl;
    }
    s_prev = ui[n - 1];

    /* 24-bit sample field (slots 4..27), 16-bit audio in the upper 16 bits */
    uint32_t s24 = ((uint32_t)(uint16_t)sample) << 8;

    /* 28 data bits: 24 sample + V + U + C + P */
    uint8_t bits[28];
    for (int i = 0; i < 24; i++) {
        bits[i] = (s24 >> i) & 1u;
    }
    bits[24] = 0; /* Validity */
    bits[25] = 0; /* User data */
    bits[26] = 0; /* Channel status (consumer, PCM) */
    uint8_t par = 0;
    for (int i = 0; i < 27; i++) {
        par ^= bits[i];
    }
    bits[27] = par; /* even parity over slots 4..31 */

    /* biphase-mark encode each data bit to 2 UI */
    for (int i = 0; i < 28; i++) {
        uint8_t u1 = !s_prev;                 /* transition at UI-pair start */
        uint8_t u2 = bits[i] ? s_prev : u1;   /* mid transition for '1' */
        ui[n++] = u1;
        ui[n++] = u2;
        s_prev = u2;
    }

    /* pack MSB-first */
    out[0] = 0;
    out[1] = 0;
    for (int i = 0; i < 64; i++) {
        if (ui[i]) {
            int w = i >> 5;
            int bit = 31 - (i & 31);
            out[w] |= (1u << bit);
        }
    }
#if SPDIF_SWAP_WORDS
    out[0] = (out[0] << 16) | (out[0] >> 16);
    out[1] = (out[1] << 16) | (out[1] >> 16);
#endif
}

/* Encode one stereo frame (L,R) into 4 x 32-bit words. */
static void build_frame(int16_t left, int16_t right, uint32_t words[4])
{
    const uint8_t *pre_l = (s_block_frame == 0) ? PREAMBLE_B : PREAMBLE_M;
    build_subframe((uint16_t)left, pre_l, &words[0]);
    build_subframe((uint16_t)right, PREAMBLE_W, &words[2]);

    if (++s_block_frame >= 192) {
        s_block_frame = 0;
    }
}

static esp_err_t spdif_i2s_start(uint32_t sample_rate_hz)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true; /* output zeros on underrun instead of stale data */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate_hz * 2, /* 2 I2S stereo frames per audio frame */
            .clk_src = I2S_CLK_SRC_APLL,          /* low jitter master clock */
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            /* BCLK/WS are generated internally but unused externally for S/PDIF;
             * route them to spare pins to satisfy the driver. */
            .bclk = GPIO_NUM_26,
            .ws = GPIO_NUM_25,
            .dout = s_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init_std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    return ESP_OK;
}

esp_err_t spdif_out_init(int gpio_dout, uint32_t sample_rate_hz)
{
    s_gpio = gpio_dout;
    s_fs = sample_rate_hz;
    s_prev = 0;
    s_block_frame = 0;
    ESP_LOGI(TAG, "init S/PDIF on GPIO%d, fs=%" PRIu32 " Hz (I2S %.1f MHz bit clock)",
             gpio_dout, sample_rate_hz, (sample_rate_hz * 128.0) / 1e6);
    return spdif_i2s_start(sample_rate_hz);
}

esp_err_t spdif_out_set_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == s_fs && s_tx) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "switch fs -> %" PRIu32 " Hz", sample_rate_hz);
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_fs = sample_rate_hz;
    s_prev = 0;
    s_block_frame = 0;
    return spdif_i2s_start(sample_rate_hz);
}

uint32_t spdif_out_get_rate(void)
{
    return s_fs;
}

void spdif_out_write(const int16_t *pcm_stereo, size_t frames)
{
    if (!s_tx) {
        return;
    }
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > SPDIF_CHUNK_FRAMES) {
            n = SPDIF_CHUNK_FRAMES;
        }
        for (size_t i = 0; i < n; i++) {
            int16_t l = pcm_stereo[2 * (done + i)];
            int16_t r = pcm_stereo[2 * (done + i) + 1];
            build_frame(l, r, &s_chunk[i * SPDIF_WORDS_PER_FRAME]);
        }
        size_t written = 0;
        i2s_channel_write(s_tx, s_chunk, n * SPDIF_WORDS_PER_FRAME * sizeof(uint32_t),
                          &written, portMAX_DELAY);
        done += n;
    }
}

void spdif_out_write_silence(size_t frames)
{
    if (!s_tx) {
        return;
    }
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > SPDIF_CHUNK_FRAMES) {
            n = SPDIF_CHUNK_FRAMES;
        }
        for (size_t i = 0; i < n; i++) {
            build_frame(0, 0, &s_chunk[i * SPDIF_WORDS_PER_FRAME]);
        }
        size_t written = 0;
        i2s_channel_write(s_tx, s_chunk, n * SPDIF_WORDS_PER_FRAME * sizeof(uint32_t),
                          &written, portMAX_DELAY);
        done += n;
    }
}
