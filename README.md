# Wireless iPhone → Chord Mojo Audio Bridge (Raspberry Pi Pico 2W)

Firmware that turns a **Raspberry Pi Pico 2W** (RP2350 + CYW43439) into a tiny,
low-power wireless audio bridge: your **iPhone streams over Bluetooth (A2DP)**,
the Pico decodes it and outputs **S/PDIF** (or I2S) into a **Chord Mojo** DAC.

This is a microcontroller port of a Raspberry Pi 3B+ prototype that used
`BlueZ → PipeWire → USB Audio host → Mojo`.

```
 iPhone  ──A2DP / SBC──▶  CYW43439  ──▶  BTstack A2DP sink  ──▶  SBC decode
                                                                    │
                                        drift-compensated buffer ◀──┘
                                                    │
                              pico_audio (S/PDIF or I2S) via PIO + DMA
                                                    │
                                     S/PDIF ─(optical/coax)─▶  Chord Mojo ─▶ headphones
```

## Why S/PDIF instead of USB?

The Pi used USB because Linux ships a USB-Audio **host** driver. No small MCU
has both the pieces this bridge needs at once:

| Chip | Bluetooth Classic (A2DP) | USB Audio **host** |
|------|--------------------------|--------------------|
| ESP32 (classic) | yes | no (UART bridge only) |
| ESP32-S3 | no (BLE only) | yes (OTG) |
| **Pico 2W (RP2350 + CYW43439)** | **yes** (BTstack) | not off-the-shelf |

Writing a UAC2 USB-host stack on an MCU is a large, high-risk effort. The Mojo,
however, natively accepts **optical TOSLINK** and **3.5 mm coax S/PDIF**, and
S/PDIF is trivial to generate from the RP2350 PIO. A2DP only carries 44.1/48 kHz,
which is well within S/PDIF limits. See [docs/wiring.md](docs/wiring.md).

## Repository layout

| Path | Purpose |
|------|---------|
| [`firmware/main.c`](firmware/main.c) | Entry point: CYW43/BT init, LED status, run loop |
| [`firmware/a2dp_sink.c`](firmware/a2dp_sink.c) | A2DP sink + AVRCP + SBC decode + drift compensation |
| [`firmware/audio_output.c`](firmware/audio_output.c) | `btstack_audio_sink_t` HAL over `pico_audio` (S/PDIF or I2S) |
| [`firmware/btstack_config.h`](firmware/btstack_config.h) | Classic-only BTstack configuration |
| [`firmware/CMakeLists.txt`](firmware/CMakeLists.txt) | Build options (backend, GPIOs, BT name) |
| [`docs/wiring.md`](docs/wiring.md) | S/PDIF coax attenuator + TOSLINK wiring, BOM |

## Prerequisites

- ARM cross toolchain: `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, `libstdc++-arm-none-eabi-newlib`
- Host build tools (for `picotool`/`pioasm`): `build-essential`, `cmake`, `libusb-1.0-0-dev`, `python3`
- The **Pico SDK** and **pico-extras** (SDK ≥ 2.0 for RP2350 / Pico 2W).

By default `CMakeLists.txt` auto-detects the SDK/extras at `~/pico/pico-sdk` and
`~/pico/pico-extras`. Otherwise pass `-DPICO_SDK_PATH=...` / `-DPICO_EXTRAS_PATH=...`
or export `PICO_SDK_PATH` / `PICO_EXTRAS_PATH`.

```bash
# one-time SDK setup (if not already present)
mkdir -p ~/pico
git clone --branch master https://github.com/raspberrypi/pico-sdk.git ~/pico/pico-sdk
git -C ~/pico/pico-sdk submodule update --init lib/btstack lib/cyw43-driver lib/lwip lib/tinyusb
git clone --branch master https://github.com/raspberrypi/pico-extras.git ~/pico/pico-extras
```

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
# -> build/firmware/mojo_bt_bridge.uf2
```

### Build options (`-D...`)

| Option | Default | Notes |
|--------|---------|-------|
| `AUDIO_OUTPUT` | `spdif` | `spdif` (Mojo coax/optical) or `i2s` (PCM5102 bring-up) |
| `SPDIF_PIN` | `18` | GPIO for the S/PDIF data line |
| `I2S_DATA_PIN` | `9` | I2S DIN (I2S mode) |
| `I2S_CLOCK_PIN_BASE` | `10` | I2S BCLK + LRCLK base (I2S mode) |
| `BT_DEVICE_NAME` | `Mojo BT Bridge` | Name shown when pairing |

Example (I2S bring-up build):

```bash
cmake -B build-i2s -S . -DAUDIO_OUTPUT=i2s -DI2S_DATA_PIN=9 -DI2S_CLOCK_PIN_BASE=10
cmake --build build-i2s -j4
```

## Flash

Hold **BOOTSEL** while plugging in the Pico 2W, then either:

```bash
picotool load -x build/firmware/mojo_bt_bridge.uf2
```

or copy `build/firmware/mojo_bt_bridge.uf2` onto the `RP2350` USB mass-storage
drive that appears.

## Use / verify (requires hardware)

1. Wire the S/PDIF output (see [docs/wiring.md](docs/wiring.md)) to the Mojo's
   optical or coax input. For first bring-up you can instead use an I2S build
   with a PCM5102 board and headphones.
2. Power the Pico. The onboard LED **slow-blinks** when it is discoverable.
3. On the iPhone: **Settings → Bluetooth**, pair with **"Mojo BT Bridge"**. LED
   goes **solid** on connect.
4. Play audio. The Mojo's sample-rate ball should light for **44.1 kHz** and you
   should hear audio. Status logs stream over USB serial (115200):
   `tail -f /dev/ttyACM0` or `picotool ... ` / any serial monitor.

Expected serial output:

```
=== Mojo BT Bridge (Pico 2W) ===
[audio] output backend: S/PDIF
[bt] BTstack up on <addr>, discoverable as "Mojo BT Bridge"
[a2dp] SBC config: 2 ch, 44100 Hz, ...
[a2dp] stream started
[avrcp] title: <song>
```

## Notes & limitations

- **Codec:** SBC only in v1 (built into BTstack). The iPhone negotiates SBC
  automatically. AAC is a possible future upgrade (see below).
- **Clock drift:** the Bluetooth source clock and the local S/PDIF clock differ
  slightly; a resampler nudges the rate based on the SBC buffer fill level.
- **Volume:** kept bit-perfect in the digital stream; use the Mojo's volume.
  AVRCP absolute-volume changes are logged.
- **Optional / future work:** AAC decode via Fraunhofer FDK-AAC (licensing +
  CPU cost) and a battery/enclosure for a portable unit — see
  [docs/wiring.md](docs/wiring.md).
