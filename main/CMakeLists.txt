idf_component_register(
    SRCS
        "main.c"
        "bt_app_core.c"
        "bt_av.c"
        "audio_render.c"
        "spdif_out.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        nvs_flash
        esp_driver_i2s
        esp_driver_gpio
        esp_ringbuf
        espressif__esp_audio_codec
)

# Bluetooth device name shown when pairing (override with -DBT_DEVICE_NAME=...)
if(NOT DEFINED BT_DEVICE_NAME)
    set(BT_DEVICE_NAME "Mojo BT Bridge")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE BT_DEVICE_NAME=\"${BT_DEVICE_NAME}\")

# AAC A2DP-sink endpoint. OFF by default: full AAC sink negotiation only exists
# on ESP-IDF master (CONFIG_BT_A2DP_CODEC_AAC_ENABLED). On stable IDF the bridge
# runs on SBC. Enable with -DMOJO_ENABLE_AAC=1 only when on a suitable IDF.
if(MOJO_ENABLE_AAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MOJO_ENABLE_AAC=1)
endif()

# AVRCP (volume/metadata) is on by default. Some ESP-IDF v6 snapshots need it
# OFF for the A2DP AAC stream to open; build with -DMOJO_ENABLE_AVRCP=0 then.
if(DEFINED MOJO_ENABLE_AVRCP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MOJO_ENABLE_AVRCP=${MOJO_ENABLE_AVRCP})
endif()
