/*
 * audio_render.h - decodes the A2DP encoded stream (AAC primary, SBC fallback)
 * to PCM and feeds the software S/PDIF output, with a buffering/drift policy.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_CODEC_NONE = 0,
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_SBC,
} audio_codec_t;

/** Start the render task and initialise the S/PDIF output. */
void audio_render_start(int spdif_gpio);

/** Select the codec + format negotiated over A2DP (called on AUDIO_CFG). */
void audio_render_set_codec(audio_codec_t codec, uint32_t sample_rate, uint8_t channels);

/** Mark the stream active/inactive (called on AUDIO_STATE started/suspended). */
void audio_render_set_active(bool active);

/** Submit one encoded frame from the BT callback (non-blocking; drops if full). */
void audio_render_submit(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
