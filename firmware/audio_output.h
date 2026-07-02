/*
 * audio_output.h
 *
 * Implements BTstack's btstack_audio_sink_t HAL on top of the pico_audio
 * library. The backend (S/PDIF or I2S) is selected at build time via the
 * AUDIO_OUTPUT_SPDIF compile definition (see firmware/CMakeLists.txt).
 */

#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include "btstack_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the BTstack audio sink instance backed by pico_audio.
 *
 * Register the returned instance with btstack_audio_sink_set_instance()
 * before starting the A2DP sink.
 */
const btstack_audio_sink_t *audio_output_get_sink_instance(void);

/**
 * @brief Human readable name of the active audio backend ("S/PDIF" or "I2S").
 */
const char *audio_output_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_OUTPUT_H
