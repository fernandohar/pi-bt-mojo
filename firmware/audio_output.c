/*
 * audio_output.c
 *
 * BTstack btstack_audio_sink_t HAL implemented on top of pico_audio.
 * Adapted from the Pico W BTstack example (btstack_audio_pico.c), extended to
 * support a S/PDIF backend (for the Chord Mojo coax/optical input) in addition
 * to plain I2S (useful for bring-up with a PCM5102 DAC).
 *
 * The pico_audio consumer runs off DMA + PIO; here we just keep its producer
 * ring topped up on a periodic BTstack timer by pulling PCM from the A2DP/SBC
 * pipeline via the registered playback callback.
 */

#define BTSTACK_FILE__ "audio_output.c"

#include "audio_output.h"

#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"

#include <stddef.h>
#include <hardware/dma.h>

#if AUDIO_OUTPUT_SPDIF
#include "pico/audio_spdif.h"
#else
#include "pico/audio_i2s.h"
#endif

#define DRIVER_POLL_INTERVAL_MS 5
#define SAMPLES_PER_BUFFER      512

// client playback callback (fills PCM into the supplied buffer)
static void (*playback_callback)(int16_t *buffer, uint16_t num_samples);

// timer to keep the output ring buffer topped up
static btstack_timer_source_t driver_timer_sink;
static bool sink_active;

static audio_format_t        output_audio_format;
static audio_buffer_format_t producer_format;
static audio_buffer_pool_t  *audio_buffer_pool;
static uint8_t               source_channel_count;

static audio_buffer_pool_t *init_audio(uint32_t sample_frequency, uint8_t channel_count) {
    source_channel_count = channel_count;

    // The pico_audio pipeline always runs stereo; mono A2DP is duplicated below.
    output_audio_format.format        = AUDIO_BUFFER_FORMAT_PCM_S16;
    output_audio_format.sample_freq   = sample_frequency;
    output_audio_format.channel_count = 2;

    producer_format.format        = &output_audio_format;
    producer_format.sample_stride = 2 * 2; // 2 channels * 2 bytes

    audio_buffer_pool_t *producer_pool =
        audio_new_producer_pool(&producer_format, 3, SAMPLES_PER_BUFFER);

#if AUDIO_OUTPUT_SPDIF
    audio_spdif_config_t config;
    config.pin         = PICO_AUDIO_SPDIF_PIN;
    config.dma_channel = (uint8_t) dma_claim_unused_channel(true);
    config.pio_sm      = 0;
    // setup claims the channel again (pico-extras issue #48), so release first
    dma_channel_unclaim(config.dma_channel);
    const audio_format_t *output_format = audio_spdif_setup(&output_audio_format, &config);
    if (!output_format) {
        panic("audio_output: unable to open S/PDIF device.\n");
    }
    bool ok = audio_spdif_connect(producer_pool);
#else
    audio_i2s_config_t config;
    config.data_pin       = PICO_AUDIO_I2S_DATA_PIN;
    config.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
    config.dma_channel    = (int8_t) dma_claim_unused_channel(true);
    config.pio_sm         = 0;
    // setup claims the channel again (pico-extras issue #48), so release first
    dma_channel_unclaim(config.dma_channel);
    const audio_format_t *output_format = audio_i2s_setup(&output_audio_format, &config);
    if (!output_format) {
        panic("audio_output: unable to open I2S device.\n");
    }
    bool ok = audio_i2s_connect(producer_pool);
#endif
    btstack_assert(ok);
    (void) ok;

    return producer_pool;
}

static void audio_set_enabled(bool enabled) {
#if AUDIO_OUTPUT_SPDIF
    audio_spdif_set_enabled(enabled);
#else
    audio_i2s_set_enabled(enabled);
#endif
}

static void sink_fill_buffers(void) {
    while (true) {
        audio_buffer_t *audio_buffer = take_audio_buffer(audio_buffer_pool, false);
        if (audio_buffer == NULL) {
            break;
        }

        int16_t *buffer16 = (int16_t *) audio_buffer->buffer->bytes;
        (*playback_callback)(buffer16, audio_buffer->max_sample_count);

        // duplicate samples for mono sources into the stereo output buffer
        if (source_channel_count == 1) {
            int16_t i;
            for (i = SAMPLES_PER_BUFFER - 1; i >= 0; i--) {
                buffer16[2 * i]     = buffer16[i];
                buffer16[2 * i + 1] = buffer16[i];
            }
        }

        audio_buffer->sample_count = audio_buffer->max_sample_count;
        give_audio_buffer(audio_buffer_pool, audio_buffer);
    }
}

static void driver_timer_handler_sink(btstack_timer_source_t *ts) {
    sink_fill_buffers();
    btstack_run_loop_set_timer(ts, DRIVER_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static int sink_init(uint8_t channels, uint32_t samplerate,
                     void (*playback)(int16_t *buffer, uint16_t num_samples)) {
    btstack_assert(playback != NULL);
    btstack_assert(channels != 0);

    playback_callback = playback;

    if (!audio_buffer_pool) {
        audio_buffer_pool = init_audio(samplerate, channels);
    }
    return 0;
}

static void sink_set_volume(uint8_t volume) {
    // The Mojo owns final volume; keep the digital stream bit-perfect.
    // AVRCP absolute-volume is still tracked in a2dp_sink.c for logging.
    UNUSED(volume);
}

static void sink_start_stream(void) {
    // pre-fill HAL buffers
    sink_fill_buffers();

    btstack_run_loop_set_timer_handler(&driver_timer_sink, &driver_timer_handler_sink);
    btstack_run_loop_set_timer(&driver_timer_sink, DRIVER_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&driver_timer_sink);

    sink_active = true;
    audio_set_enabled(true);
}

static void sink_stop_stream(void) {
    audio_set_enabled(false);
    btstack_run_loop_remove_timer(&driver_timer_sink);
    sink_active = false;
}

static void sink_close(void) {
    if (sink_active) {
        sink_stop_stream();
    }
}

static const btstack_audio_sink_t audio_output_sink = {
    .init         = &sink_init,
    .set_volume   = &sink_set_volume,
    .start_stream = &sink_start_stream,
    .stop_stream  = &sink_stop_stream,
    .close        = &sink_close,
};

const btstack_audio_sink_t *audio_output_get_sink_instance(void) {
    return &audio_output_sink;
}

const char *audio_output_backend_name(void) {
#if AUDIO_OUTPUT_SPDIF
    return "S/PDIF";
#else
    return "I2S";
#endif
}
