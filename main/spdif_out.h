/*
 * spdif_out.h - software S/PDIF output on the ESP32 I2S peripheral.
 *
 * Generates a biphase-mark-coded (BMC) S/PDIF bitstream on a single GPIO by
 * running the I2S peripheral at 128x the audio sample rate and streaming
 * pre-encoded 32-bit words. Feed it 16-bit stereo PCM; wire the output GPIO to
 * a TOSLINK transmitter or a coax attenuator (see docs/wiring.md).
 *
 * NOTE: the exact bit/word ordering of the ESP32 I2S peripheral in 32-bit mode
 * must be validated on real hardware (scope / DAC lock). See SPDIF_SWAP_WORDS.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise the S/PDIF output on the given GPIO at the given sample rate. */
esp_err_t spdif_out_init(int gpio_dout, uint32_t sample_rate_hz);

/** Change the S/PDIF sample rate (re-inits the I2S clock). No-op if unchanged. */
esp_err_t spdif_out_set_rate(uint32_t sample_rate_hz);

/** Encode and output `frames` 16-bit stereo PCM samples (interleaved L,R). */
void spdif_out_write(const int16_t *pcm_stereo, size_t frames);

/** Output `frames` of digital silence (keeps the receiver locked during gaps). */
void spdif_out_write_silence(size_t frames);

/** Current configured audio sample rate (Hz). */
uint32_t spdif_out_get_rate(void);

#ifdef __cplusplus
}
#endif
