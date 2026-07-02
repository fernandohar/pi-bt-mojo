cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# Wireless iPhone -> Chord Mojo audio bridge (Raspberry Pi Pico 2W)
#
# Pipeline:  iPhone --A2DP/SBC--> CYW43439 --> BTstack A2DP sink --> SBC decode
#            --> drift-compensated buffer --> pico_audio (S/PDIF or I2S) --> Mojo
# ---------------------------------------------------------------------------

# Target board: Raspberry Pi Pico 2W (RP2350 + CYW43439).
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Enable Bluetooth in the CYW43 driver (BR/EDR classic is needed for A2DP).
set(CYW43_ENABLE_BLUETOOTH 1)

# ---------------------------------------------------------------------------
# Locate the Pico SDK and pico-extras.
#
# Order of preference:
#   1. -DPICO_SDK_PATH / -DPICO_EXTRAS_PATH on the cmake command line
#   2. PICO_SDK_PATH / PICO_EXTRAS_PATH environment variables
#   3. The default clone locations used by the setup / update script (~/pico/...)
#
# Auto-detecting (3) means the project builds with a plain `cmake -B build`
# without needing any environment variables exported in the shell.
# ---------------------------------------------------------------------------
if (NOT PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{HOME}/pico/pico-sdk")
    set(PICO_SDK_PATH "$ENV{HOME}/pico/pico-sdk")
endif()
if (NOT PICO_EXTRAS_PATH AND NOT DEFINED ENV{PICO_EXTRAS_PATH} AND EXISTS "$ENV{HOME}/pico/pico-extras")
    set(PICO_EXTRAS_PATH "$ENV{HOME}/pico/pico-extras")
endif()

include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(mojo_bt_bridge C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(firmware)
