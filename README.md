# Wireless iPhone → Chord Mojo Audio Bridge (ESP32-WROOM)

Firmware that turns an **ESP32-WROOM** into a tiny wireless audio bridge: your
**iPhone streams over Bluetooth (A2DP, AAC)**, the ESP32 decodes it and outputs
**S/PDIF** into a **Chord Mojo** DAC.

This is a microcontroller port of a Raspberry Pi 3B+ prototype
(`BlueZ → PipeWire → USB Audio host → Mojo`). It targets the classic ESP32
specifically to gain **AAC** — the higher-quality codec the iPhone actually uses.

```
 iPhone ──A2DP/AAC──▶ ESP32 Bluedroid A2DP sink (external codec)
        ──▶ AAC decode (esp_audio_codec) / SBC fallback
        ──▶ ring buffer + drift policy
        ──▶ software I2S → S/PDIF (biphase-mark) ──▶ Chord Mojo ──▶ headphones
```

## Why ESP32-WROOM + AAC (and why still S/PDIF)

- The **iPhone only uses AAC or SBC** over A2DP (never aptX/LDAC — Apple doesn't
  license them). AAC is the meaningful quality upgrade over SBC.
- The classic **ESP32-WROOM has Bluetooth Classic** (needed for A2DP) and, via
  the recent ESP-IDF **external-codec A2DP sink** API, can hand raw AAC frames to
  the app, where **`esp_audio_codec`** decodes them.
- The ESP32 has **no USB host**, so — like the Pico approach — it reaches the
  Mojo over **S/PDIF** (the Mojo natively accepts optical/coax). S/PDIF is
  generated in software from the I2S peripheral. See [docs/wiring.md](docs/wiring.md).

## Repository layout

| Path | Purpose |
|------|---------|
| [`main/main.c`](main/main.c) | App entry: NVS, classic BT bring-up, LED status, task startup |
| [`main/bt_av.c`](main/bt_av.c) | GAP (SSP) + A2DP sink (external codec) + AVRCP; registers AAC+SBC endpoints |
| [`main/audio_render.c`](main/audio_render.c) | Encoded-frame ring buffer + AAC/SBC decode + drift policy |
| [`main/spdif_out.c`](main/spdif_out.c) | Clean-room biphase-mark S/PDIF encoder over I2S |
| [`main/bt_app_core.c`](main/bt_app_core.c) | Work-dispatch task for Bluedroid callbacks |
| [`sdkconfig.defaults`](sdkconfig.defaults) | BR/EDR-only, A2DP external codec, 240 MHz |
| [`docs/wiring.md`](docs/wiring.md) | S/PDIF wiring (coax + TOSLINK), optional WM8804, BOM |

## Prerequisites

- **ESP-IDF v5.5.x** (installed to `~/esp/esp-idf` by the setup/update script)
  with the esp32 toolchain. Source it once per shell:
  ```bash
  . ~/esp/esp-idf/export.sh
  ```
- The **`espressif/esp_audio_codec`** managed component (fetched automatically by
  the IDF component manager on first `reconfigure`/`build`).

## Build

```bash
. ~/esp/esp-idf/export.sh          # once per shell
idf.py set-target esp32            # once (generates sdkconfig from defaults)
idf.py build                       # -> build/mojo_bt_bridge.bin
```

Optional overrides (compile definitions in [`main/CMakeLists.txt`](main/CMakeLists.txt)
and headers): `SPDIF_GPIO` (default **27**), `STATUS_LED_GPIO` (default **2**),
`BT_DEVICE_NAME` (default `Mojo BT Bridge`).

## Flash & monitor

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Use / verify (requires hardware)

1. Wire the S/PDIF output on **GPIO27** to the Mojo's optical or coax input
   (see [docs/wiring.md](docs/wiring.md)).
2. Power the ESP32. The status LED **blinks** while discoverable.
3. On the iPhone: **Settings → Bluetooth**, pair with **"Mojo BT Bridge"**. The
   LED goes **solid** on connect.
4. Play audio. Expected serial log:
   ```
   === Mojo BT Bridge (ESP32-WROOM) ===
   [bt_av] discoverable as "Mojo BT Bridge" - pair from the iPhone
   [bt_av] codec configured: AAC (assuming 44100 Hz stereo AAC-LC)
   [render] opened AAC decoder (44100 Hz, 2 ch)
   [spdif] init S/PDIF on GPIO27, fs=44100 Hz ...
   ```
   The Mojo's sample-rate ball should light for **44.1 kHz** and play audio.

## Status & limitations

- **Codec:** AAC primary (iPhone), SBC fallback — both decoded by
  `esp_audio_codec`. aptX/LDAC are intentionally out of scope (iPhone never uses them).
- **S/PDIF is software-generated** (see recommendation in [docs/wiring.md](docs/wiring.md)).
  The BMC bit/word ordering of the ESP32 I2S peripheral should be confirmed on a
  scope/DAC; a `SPDIF_SWAP_WORDS` compile switch is provided for the known
  32-bit half-word-swap quirk.
- **Drift:** simple policy (drop encoded frames on overflow, insert silence on
  underflow); the S/PDIF clock follows the decoder's reported rate. Fractional
  resampling is a future refinement.
- **AAC framing:** A2DP AAC is treated as raw AAC-LC (no ADTS) at 44.1 kHz
  stereo, matching the iPhone. Non-Apple / non-44.1 sources would need the M24
  codec-capability element parsed in [`bt_av.c`](main/bt_av.c).
- Hardware-in-the-loop (real iPhone pairing + Mojo lock) can't run in CI; the
  in-repo gate is a clean `idf.py build`.
