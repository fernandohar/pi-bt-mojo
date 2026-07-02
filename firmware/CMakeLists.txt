# ---------------------------------------------------------------------------
# mojo_bt_bridge firmware target
# ---------------------------------------------------------------------------

# Audio output backend: "spdif" (recommended, drives the Mojo coax/optical
# input) or "i2s" (handy for bring-up with a cheap PCM5102 I2S DAC).
set(AUDIO_OUTPUT "spdif" CACHE STRING "Audio output backend: spdif or i2s")
set_property(CACHE AUDIO_OUTPUT PROPERTY STRINGS spdif i2s)

# Default GPIO assignments (override with -D... on the cmake command line).
#   S/PDIF: single data pin feeding a TOSLINK TX module or coax attenuator.
#   I2S:    DATA pin + (BCLK, LRCLK) on CLOCK_PIN_BASE and CLOCK_PIN_BASE+1.
set(SPDIF_PIN        18 CACHE STRING "GPIO for S/PDIF output")
set(I2S_DATA_PIN      9 CACHE STRING "GPIO for I2S DATA (DIN)")
set(I2S_CLOCK_PIN_BASE 10 CACHE STRING "GPIO base for I2S BCLK/LRCLK")

set(BT_DEVICE_NAME "Mojo BT Bridge" CACHE STRING "Bluetooth device name shown to the iPhone")

add_executable(mojo_bt_bridge
    main.c
    a2dp_sink.c
    audio_output.c
    # BTstack audio HAL registry (set/get instance) is not part of the base lib
    ${PICO_SDK_PATH}/lib/btstack/src/btstack_audio.c
)

target_include_directories(mojo_bt_bridge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}   # our btstack_config.h + headers
)

target_compile_definitions(mojo_bt_bridge PRIVATE
    CYW43_ENABLE_BLUETOOTH=1
    CYW43_LWIP=0                 # BT only, no networking -> no lwipopts.h needed
    # RP2350 requires explicit audio spinlock IDs (see pico/audio.h). 6 and 7 are
    # in the general-purpose range (SDK claims runtime spinlocks from 24-31).
    SPINLOCK_ID_AUDIO_FREE_LIST_LOCK=6
    SPINLOCK_ID_AUDIO_PREPARED_LISTS_LOCK=7
    BT_DEVICE_NAME=\"${BT_DEVICE_NAME}\"
)

target_link_libraries(mojo_bt_bridge PRIVATE
    pico_stdlib
    pico_cyw43_arch_threadsafe_background
    pico_btstack_classic
    pico_btstack_cyw43
    pico_btstack_sbc_decoder
)

if (AUDIO_OUTPUT STREQUAL "i2s")
    message(STATUS "mojo_bt_bridge: audio output = I2S (DATA=${I2S_DATA_PIN}, CLOCK_BASE=${I2S_CLOCK_PIN_BASE})")
    target_link_libraries(mojo_bt_bridge PRIVATE pico_audio_i2s)
    target_compile_definitions(mojo_bt_bridge PRIVATE
        AUDIO_OUTPUT_SPDIF=0
        PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLOCK_PIN_BASE}
    )
else()
    message(STATUS "mojo_bt_bridge: audio output = S/PDIF (PIN=${SPDIF_PIN})")
    target_link_libraries(mojo_bt_bridge PRIVATE pico_audio_spdif)
    target_compile_definitions(mojo_bt_bridge PRIVATE
        AUDIO_OUTPUT_SPDIF=1
        PICO_AUDIO_SPDIF_PIN=${SPDIF_PIN}
    )
endif()

# Log over USB CDC (and UART). Useful for watching pairing / stream status.
pico_enable_stdio_usb(mojo_bt_bridge 1)
pico_enable_stdio_uart(mojo_bt_bridge 1)

# Produce .uf2 / .elf.map / etc.
pico_add_extra_outputs(mojo_bt_bridge)
