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
