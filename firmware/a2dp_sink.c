/*
 * a2dp_sink.c
 *
 * A2DP Sink + AVRCP for the wireless iPhone -> Chord Mojo bridge.
 *
 * Adapted from the BTstack a2dp_sink_demo (BlueKitchen GmbH, BSD-3), trimmed
 * for an embedded audio bridge:
 *   - SBC frames are buffered and decoded to PCM on demand
 *   - a software resampler nudges the effective rate to compensate for drift
 *     between the Bluetooth source clock and the local S/PDIF/I2S clock
 *   - decoded PCM is handed to the registered btstack_audio_sink (S/PDIF/I2S)
 *   - AVRCP controller/target track playback status and absolute volume
 *
 * Removed vs. the upstream demo: WAV capture, cover-art client, and the
 * interactive stdin console.
 */

#define BTSTACK_FILE__ "a2dp_sink.c"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "btstack_resample.h"
#include "btstack_ring_buffer.h"

#include "a2dp_sink.h"

#ifndef BT_DEVICE_NAME
#define BT_DEVICE_NAME "Mojo BT Bridge"
#endif

#define NUM_CHANNELS        2
#define BYTES_PER_FRAME     (2 * NUM_CHANNELS)
#define MAX_SBC_FRAME_SIZE  120

static btstack_packet_callback_registration_t hci_event_callback_registration;

static uint8_t sdp_avdtp_sink_service_buffer[150];
static uint8_t sdp_avrcp_target_service_buffer[150];
static uint8_t sdp_avrcp_controller_service_buffer[200];
static uint8_t device_id_sdp_service_buffer[100];

// we support all SBC configurations with bitpool 2-53
static uint8_t media_sbc_codec_capabilities[] = {
    0xFF, // all sampling frequencies / channel modes
    0xFF, // all block lengths / subbands / allocation methods
    2, 53
};

// SBC Decoder
static const btstack_sbc_decoder_t *   sbc_decoder_instance;
static btstack_sbc_decoder_bluedroid_t sbc_decoder_context;

// ring buffer for SBC frames
// below OPTIMAL_FRAMES_MIN: stretch; within [MIN,MAX]: nominal; above: compress
#define OPTIMAL_FRAMES_MIN 60
#define OPTIMAL_FRAMES_MAX 80
#define ADDITIONAL_FRAMES  30
static uint8_t sbc_frame_storage[(OPTIMAL_FRAMES_MAX + ADDITIONAL_FRAMES) * MAX_SBC_FRAME_SIZE];
static btstack_ring_buffer_t sbc_frame_ring_buffer;
static unsigned int sbc_frame_size;

// overflow buffer for decoded PCM not consumed in a single request
static uint8_t decoded_audio_storage[(128 + 16) * BYTES_PER_FRAME];
static btstack_ring_buffer_t decoded_audio_ring_buffer;

static int media_initialized = 0;
static int audio_stream_started;
static btstack_resample_t resample_instance;

// temp storage of lower-layer request for audio samples
static int16_t *request_buffer;
static int      request_frames;

// sink state
static int volume_percentage = 0;
static avrcp_battery_status_t battery_status = AVRCP_BATTERY_STATUS_WARNING;

typedef struct {
    uint8_t  reconfigure;
    uint8_t  num_channels;
    uint16_t sampling_frequency;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  min_bitpool_value;
    uint8_t  max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

typedef enum {
    STREAM_STATE_CLOSED,
    STREAM_STATE_OPEN,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED,
} stream_state_t;

typedef struct {
    uint8_t a2dp_local_seid;
    uint8_t media_sbc_codec_configuration[4];
} a2dp_sink_stream_endpoint_t;
static a2dp_sink_stream_endpoint_t stream_endpoint_storage;

typedef struct {
    bd_addr_t addr;
    uint16_t  a2dp_cid;
    uint8_t   a2dp_local_seid;
    stream_state_t stream_state;
    media_codec_configuration_sbc_t sbc_configuration;
} a2dp_connection_t;
static a2dp_connection_t a2dp_connection;

typedef struct {
    bd_addr_t addr;
    uint16_t  avrcp_cid;
    bool playing;
    uint16_t notifications_supported_by_target;
} bridge_avrcp_connection_t;
static bridge_avrcp_connection_t avrcp_connection;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t event_size);
static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size);
static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ----------------------------------------------------------------------------
// Media processing: SBC frame buffering, decoding and drift compensation
// ----------------------------------------------------------------------------

static void playback_handler(int16_t *buffer, uint16_t num_audio_frames) {
    // called from the audio HAL, guaranteed to be on the BTstack main thread
    if (sbc_frame_size == 0) {
        memset(buffer, 0, num_audio_frames * BYTES_PER_FRAME);
        return;
    }

    // first drain any previously decoded PCM
    uint32_t bytes_read;
    btstack_ring_buffer_read(&decoded_audio_ring_buffer, (uint8_t *) buffer,
                             num_audio_frames * BYTES_PER_FRAME, &bytes_read);
    buffer           += bytes_read / NUM_CHANNELS;
    num_audio_frames -= bytes_read / BYTES_PER_FRAME;

    // then decode more SBC frames into the request buffer via handle_pcm_data
    request_buffer = buffer;
    request_frames = num_audio_frames;
    while (request_frames && btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) >= sbc_frame_size) {
        uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
        btstack_ring_buffer_read(&sbc_frame_ring_buffer, sbc_frame, sbc_frame_size, &bytes_read);
        sbc_decoder_instance->decode_signed_16(&sbc_decoder_context, 0, sbc_frame, sbc_frame_size);
    }
}

static void handle_pcm_data(int16_t *data, int num_audio_frames, int num_channels, int sample_rate, void *context) {
    UNUSED(sample_rate);
    UNUSED(context);
    UNUSED(num_channels); // stereo == 2

    // resample into an intermediate buffer (extra headroom for stretch)
    int16_t  output_buffer[(128 + 16) * NUM_CHANNELS];
    uint32_t resampled_frames = btstack_resample_block(&resample_instance, data, num_audio_frames, output_buffer);

    // satisfy the pending request first
    int frames_to_copy = btstack_min(resampled_frames, (uint32_t) request_frames);
    memcpy(request_buffer, output_buffer, frames_to_copy * BYTES_PER_FRAME);
    request_frames -= frames_to_copy;
    request_buffer += frames_to_copy * NUM_CHANNELS;

    // stash the remainder for the next request
    int frames_to_store = resampled_frames - frames_to_copy;
    if (frames_to_store) {
        int status = btstack_ring_buffer_write(&decoded_audio_ring_buffer,
                                               (uint8_t *) &output_buffer[frames_to_copy * NUM_CHANNELS],
                                               frames_to_store * BYTES_PER_FRAME);
        if (status) {
            printf("[audio] PCM ring buffer overrun\n");
        }
    }
}

static int media_processing_init(media_codec_configuration_sbc_t *configuration) {
    if (media_initialized) return 0;

    sbc_decoder_instance = btstack_sbc_decoder_bluedroid_init_instance(&sbc_decoder_context);
    sbc_decoder_instance->configure(&sbc_decoder_context, SBC_MODE_STANDARD, handle_pcm_data, NULL);

    btstack_ring_buffer_init(&sbc_frame_ring_buffer, sbc_frame_storage, sizeof(sbc_frame_storage));
    btstack_ring_buffer_init(&decoded_audio_ring_buffer, decoded_audio_storage, sizeof(decoded_audio_storage));
    btstack_resample_init(&resample_instance, configuration->num_channels);

    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->init(NUM_CHANNELS, configuration->sampling_frequency, &playback_handler);
    }

    audio_stream_started = 0;
    media_initialized = 1;
    return 0;
}

static void media_processing_start(void) {
    if (!media_initialized) return;
    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->start_stream();
    }
    audio_stream_started = 1;
}

static void media_processing_pause(void) {
    if (!media_initialized) return;
    audio_stream_started = 0;
    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->stop_stream();
    }
    btstack_ring_buffer_reset(&decoded_audio_ring_buffer);
    btstack_ring_buffer_reset(&sbc_frame_ring_buffer);
}

static void media_processing_close(void) {
    if (!media_initialized) return;
    media_initialized = 0;
    audio_stream_started = 0;
    sbc_frame_size = 0;

    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->close();
    }
}

// ----------------------------------------------------------------------------
// AVDTP media packet handling
// ----------------------------------------------------------------------------

static int read_sbc_header(uint8_t *packet, int size, int *offset, avdtp_sbc_codec_header_t *sbc_header) {
    int sbc_header_len = 12; // without crc
    int pos = *offset;
    if (size - pos < sbc_header_len) {
        printf("[a2dp] not enough data for SBC header\n");
        return 0;
    }
    sbc_header->fragmentation   = get_bit16(packet[pos], 7);
    sbc_header->starting_packet = get_bit16(packet[pos], 6);
    sbc_header->last_packet     = get_bit16(packet[pos], 5);
    sbc_header->num_frames      = packet[pos] & 0x0f;
    pos++;
    *offset = pos;
    return 1;
}

static int read_media_data_header(uint8_t *packet, int size, int *offset, avdtp_media_packet_header_t *media_header) {
    int media_header_len = 12; // without crc
    int pos = *offset;
    if (size - pos < media_header_len) {
        printf("[a2dp] not enough data for media header\n");
        return 0;
    }
    media_header->version    = packet[pos] & 0x03;
    media_header->padding    = get_bit16(packet[pos], 2);
    media_header->extension  = get_bit16(packet[pos], 3);
    media_header->csrc_count = (packet[pos] >> 4) & 0x0F;
    pos++;
    media_header->marker       = get_bit16(packet[pos], 0);
    media_header->payload_type = (packet[pos] >> 1) & 0x7F;
    pos++;
    media_header->sequence_number = big_endian_read_16(packet, pos);
    pos += 2;
    media_header->timestamp = big_endian_read_32(packet, pos);
    pos += 4;
    media_header->synchronization_source = big_endian_read_32(packet, pos);
    pos += 4;
    *offset = pos;
    return 1;
}

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size) {
    UNUSED(seid);
    int pos = 0;

    avdtp_media_packet_header_t media_header;
    if (!read_media_data_header(packet, size, &pos, &media_header)) return;

    avdtp_sbc_codec_header_t sbc_header;
    if (!read_sbc_header(packet, size, &pos, &sbc_header)) return;

    int      packet_length = size - pos;
    uint8_t *packet_begin  = packet + pos;

    const btstack_audio_sink_t *audio = btstack_audio_sink_get_instance();
    if (!audio) {
        // no audio backend: decode-and-drop to keep the decoder in sync
        sbc_decoder_instance->decode_signed_16(&sbc_decoder_context, 0, packet_begin, packet_length);
        return;
    }

    sbc_frame_size = packet_length / sbc_header.num_frames;
    int status = btstack_ring_buffer_write(&sbc_frame_ring_buffer, packet_begin, packet_length);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[a2dp] SBC ring buffer overrun\n");
    }

    // drift compensation: nudge resampling factor by SBC buffer fill level
    int sbc_frames_in_buffer = btstack_ring_buffer_bytes_available(&sbc_frame_ring_buffer) / sbc_frame_size;

    uint32_t nominal_factor = 0x10000;
    uint32_t compensation   = 0x00100;
    uint32_t resampling_factor;
    if (sbc_frames_in_buffer < OPTIMAL_FRAMES_MIN) {
        resampling_factor = nominal_factor - compensation; // stretch (source too slow)
    } else if (sbc_frames_in_buffer <= OPTIMAL_FRAMES_MAX) {
        resampling_factor = nominal_factor;                // nominal
    } else {
        resampling_factor = nominal_factor + compensation; // compress (source too fast)
    }
    btstack_resample_set_factor(&resample_instance, resampling_factor);

    if (!audio_stream_started && sbc_frames_in_buffer >= OPTIMAL_FRAMES_MIN) {
        media_processing_start();
    }
}

static void dump_sbc_configuration(media_codec_configuration_sbc_t *configuration) {
    printf("[a2dp] SBC config: %u ch, %u Hz, block %u, subbands %u, bitpool [%u..%u]\n",
           configuration->num_channels, configuration->sampling_frequency,
           configuration->block_length, configuration->subbands,
           configuration->min_bitpool_value, configuration->max_bitpool_value);
}

// ----------------------------------------------------------------------------
// HCI / A2DP / AVRCP event handlers
// ----------------------------------------------------------------------------

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t address;
        printf("[bt] pin code request - using '0000'\n");
        hci_event_pin_code_request_get_bd_addr(packet, address);
        gap_pin_code_response(address, "0000");
    }
}

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    uint16_t  local_cid;
    uint8_t   status;
    bd_addr_t address;

    bridge_avrcp_connection_t *connection = &avrcp_connection;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[avrcp] connection failed, status 0x%02x\n", status);
                connection->avrcp_cid = 0;
                return;
            }
            connection->avrcp_cid = local_cid;
            avrcp_subevent_connection_established_get_bd_addr(packet, address);
            printf("[avrcp] connected to %s, cid 0x%02x\n", bd_addr_to_str(address), connection->avrcp_cid);

            avrcp_target_support_event(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(connection->avrcp_cid, battery_status);
            avrcp_controller_get_supported_events(connection->avrcp_cid);
            return;

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            printf("[avrcp] channel released: cid 0x%02x\n",
                   avrcp_subevent_connection_released_get_avrcp_cid(packet));
            connection->avrcp_cid = 0;
            connection->notifications_supported_by_target = 0;
            return;
        default:
            break;
    }
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    uint8_t avrcp_subevent_value[256];
    uint8_t play_status;

    bridge_avrcp_connection_t *connection = &avrcp_connection;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (connection->avrcp_cid == 0) return;

    memset(avrcp_subevent_value, 0, sizeof(avrcp_subevent_value));
    switch (packet[2]) {
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID:
            connection->notifications_supported_by_target |=
                (1 << avrcp_subevent_get_capability_event_id_get_event_id(packet));
            break;
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
            avrcp_controller_enable_notification(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            play_status = avrcp_subevent_notification_playback_status_changed_get_play_status(packet);
            printf("[avrcp] playback status: %s\n", avrcp_play_status2str(play_status));
            connection->playing = (play_status == AVRCP_PLAYBACK_STATUS_PLAYING);
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            if (avrcp_subevent_now_playing_title_info_get_value_len(packet) > 0) {
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_title_info_get_value(packet),
                       avrcp_subevent_now_playing_title_info_get_value_len(packet));
                printf("[avrcp] title: %s\n", avrcp_subevent_value);
            }
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            if (avrcp_subevent_now_playing_artist_info_get_value_len(packet) > 0) {
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_artist_info_get_value(packet),
                       avrcp_subevent_now_playing_artist_info_get_value_len(packet));
                printf("[avrcp] artist: %s\n", avrcp_subevent_value);
            }
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            if (avrcp_subevent_now_playing_album_info_get_value_len(packet) > 0) {
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_album_info_get_value(packet),
                       avrcp_subevent_now_playing_album_info_get_value_len(packet));
                printf("[avrcp] album: %s\n", avrcp_subevent_value);
            }
            break;
        default:
            break;
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    uint8_t volume;

    switch (packet[2]) {
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            volume_percentage = volume * 100 / 127;
            printf("[avrcp] volume set to %d%% (%d)\n", volume_percentage, volume);
            // The Mojo controls analog volume; we keep the digital stream intact.
            break;
        default:
            break;
    }
}

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    uint8_t allocation_method;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    a2dp_connection_t *conn = &a2dp_connection;

    switch (packet[2]) {
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            printf("[a2dp] received non-SBC codec - not supported\n");
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            conn->sbc_configuration.reconfigure        = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            conn->sbc_configuration.num_channels       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            conn->sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            conn->sbc_configuration.block_length       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            conn->sbc_configuration.subbands           = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            conn->sbc_configuration.min_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            conn->sbc_configuration.max_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);

            allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            conn->sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(allocation_method - 1);

            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)) {
                case AVDTP_CHANNEL_MODE_JOINT_STEREO:
                    conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_STEREO:
                    conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
                    conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
                    break;
                case AVDTP_CHANNEL_MODE_MONO:
                    conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            dump_sbc_configuration(&conn->sbc_configuration);
            break;

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[a2dp] streaming connection failed, status 0x%02x\n", status);
                break;
            }
            a2dp_subevent_stream_established_get_bd_addr(packet, conn->addr);
            conn->a2dp_cid        = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            conn->a2dp_local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            conn->stream_state    = STREAM_STATE_OPEN;
            printf("[a2dp] streaming established with %s, cid 0x%02x\n",
                   bd_addr_to_str(conn->addr), conn->a2dp_cid);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            printf("[a2dp] stream started\n");
            conn->stream_state = STREAM_STATE_PLAYING;
            if (conn->sbc_configuration.reconfigure) {
                media_processing_close();
            }
            media_processing_init(&conn->sbc_configuration);
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            printf("[a2dp] stream paused\n");
            conn->stream_state = STREAM_STATE_PAUSED;
            media_processing_pause();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            printf("[a2dp] stream released\n");
            conn->stream_state = STREAM_STATE_CLOSED;
            media_processing_close();
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            printf("[a2dp] signaling connection released\n");
            conn->a2dp_cid = 0;
            media_processing_close();
            break;

        default:
            break;
    }
}

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------

int a2dp_sink_setup(void) {
    // init protocols
    l2cap_init();
    sdp_init();

    // init profiles
    a2dp_sink_init();
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    // configure A2DP sink
    a2dp_sink_register_packet_handler(&a2dp_sink_packet_handler);
    a2dp_sink_register_media_handler(&handle_l2cap_media_data_packet);

    avdtp_stream_endpoint_t *local_stream_endpoint = a2dp_sink_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC,
        media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
        stream_endpoint_storage.media_sbc_codec_configuration,
        sizeof(stream_endpoint_storage.media_sbc_codec_configuration));
    btstack_assert(local_stream_endpoint != NULL);
    stream_endpoint_storage.a2dp_local_seid = avdtp_local_seid(local_stream_endpoint);

    // configure AVRCP controller + target
    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // SDP records
    memset(sdp_avdtp_sink_service_buffer, 0, sizeof(sdp_avdtp_sink_service_buffer));
    a2dp_sink_create_sdp_record(sdp_avdtp_sink_service_buffer, sdp_create_service_record_handle(),
                                AVDTP_SINK_FEATURE_MASK_HEADPHONE, NULL, NULL);
    btstack_assert(de_get_len(sdp_avdtp_sink_service_buffer) <= sizeof(sdp_avdtp_sink_service_buffer));
    sdp_register_service(sdp_avdtp_sink_service_buffer);

    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = 1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER;
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       controller_supported_features, NULL, NULL);
    btstack_assert(de_get_len(sdp_avrcp_controller_service_buffer) <= sizeof(sdp_avrcp_controller_service_buffer));
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t target_supported_features = 1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER;
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(),
                                   target_supported_features, NULL, NULL);
    btstack_assert(de_get_len(sdp_avrcp_target_service_buffer) <= sizeof(sdp_avrcp_target_service_buffer));
    sdp_register_service(sdp_avrcp_target_service_buffer);

    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    btstack_assert(de_get_len(device_id_sdp_service_buffer) <= sizeof(device_id_sdp_service_buffer));
    sdp_register_service(device_id_sdp_service_buffer);

    // GAP - discoverable audio sink
    gap_set_local_name(BT_DEVICE_NAME " 00:00:00:00:00:00");
    gap_discoverable_control(1);
    // Service Class: Audio, Major Device Class: Audio, Minor: Headphone
    gap_set_class_of_device(0x200404);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

    // register for HCI events (legacy pairing pin)
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    return 0;
}
