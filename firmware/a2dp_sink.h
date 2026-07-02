/*
 * a2dp_sink.h
 *
 * A2DP sink + AVRCP setup for the Mojo BT bridge. Receives an SBC audio
 * stream from an A2DP source (e.g. an iPhone), decodes it to PCM and feeds
 * the registered BTstack audio sink (see audio_output.h).
 */

#ifndef BRIDGE_A2DP_SINK_H
#define BRIDGE_A2DP_SINK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure L2CAP/SDP/A2DP sink/AVRCP and GAP.
 *
 * Call once after cyw43_arch_init() and after registering the audio sink
 * instance, but before hci_power_control(HCI_POWER_ON).
 *
 * @return 0 on success
 */
int a2dp_sink_setup(void);

#ifdef __cplusplus
}
#endif

#endif // BRIDGE_A2DP_SINK_H
