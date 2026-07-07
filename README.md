# Wireless iPhone → Chord Mojo Audio Bridge (ESP32-WROOM)

Firmware that turns an **ESP32-WROOM** into a tiny wireless audio bridge: your
**iPhone streams over Bluetooth (A2DP, AAC)**, the ESP32 decodes it and outputs
**S/PDIF** into a **Chord Mojo** DAC.

This is a microcontroller port of a Raspberry Pi 3B+ prototype
(`BlueZ → PipeWire → USB Audio host → Mojo`). It targets the classic ESP32 to
pursue **AAC** (the iPhone's higher-quality codec). Build on **ESP-IDF v6+**,
which supports both **AAC** and **SBC** A2DP-sink negotiation (both verified
working on hardware). aptX/LDAC don't apply (the iPhone never uses them).

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

## Build & flash on macOS (Apple Silicon / M1)

Full from-scratch setup on a MacBook (M1/M2, macOS with Python ≥ 3.9).

### 1. Install the prerequisites (Homebrew)

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"  # if you don't have brew
brew install cmake ninja dfu-util python3 git
```

### 2. Install ESP-IDF v6 (pick ONE)

AAC A2DP-sink support requires **ESP-IDF v6+**, so v6 is the recommended version
(it does SBC too). Verified on **ESP-IDF v6.2.0**.

**Option A - VS Code (recommended, GUI):**
1. Install [VS Code](https://code.visualstudio.com/) (Apple Silicon build).
2. Install the **"Espressif IDF"** extension from the Marketplace.
3. Run command palette → **"ESP-IDF: Configure ESP-IDF Extension"** → *Express* → choose **v6.x** (or `master`) and target **esp32**. It downloads the toolchain for you.

**Option B - Command line:**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b release/v6.0 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6
cd ~/esp/esp-idf-v6 && ./install.sh esp32
```

Then source it **per terminal** when you want to build:

```bash
. ~/esp/esp-idf-v6/export.sh
```

Prefer an alias over auto-sourcing in `~/.zshrc` (auto-sourcing in every shell
makes it hard to keep more than one IDF around):

```bash
# in ~/.zshrc
alias idf6='. $HOME/esp/esp-idf-v6/export.sh'   # v6 (AAC + SBC) - recommended
```

Then run `idf6` once in each new terminal. If you also keep an older IDF (e.g.
v5.5.x, SBC-only) around, never source two versions in one shell — their Python
environments conflict.

### 3. Get the source and build

```bash
git clone https://github.com/fernandohar/pi-bt-mojo.git
cd pi-bt-mojo
. ~/esp/esp-idf-v6/export.sh       # if not auto-loaded (VS Code does this for you)
idf.py set-target esp32
idf.py build
```

This builds the default **SBC** firmware. To also offer **AAC** to the iPhone,
see [Building for AAC](#building-for-aac-esp-idf-v6) (same v6 toolchain, two extra
build flags).

### 4. USB driver + connect the ESP32

Most ESP32 dev boards use a **CP2102** (Silicon Labs) or **CH340/CH9102** (WCH) USB-UART chip:
- **CP2102:** usually works on modern macOS; if not, install the [Silicon Labs CP210x VCP driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).
- **CH340/CH9102:** install the [WCH macOS driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html).

Plug the board in with a **data-capable USB cable** (not charge-only), then find the port:

```bash
ls /dev/cu.*        # e.g. /dev/cu.usbserial-0001, /dev/cu.SLAB_USBtoUART, /dev/cu.wchusbserial*
```

### 5. Flash and watch the logs

```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor     # use your port; Ctrl-] to exit monitor
```

(VS Code: pick the port in the bottom bar, then the flame **Flash** and plug **Monitor** buttons.)

Then wire the S/PDIF output (**GPIO27**) to the Mojo per [docs/wiring.md](docs/wiring.md),
pair the iPhone with **"Mojo BT Bridge"**, and play.

## Prerequisites

- **ESP-IDF v6+** (installed to `~/esp/esp-idf-v6`) with the esp32 toolchain.
  Source it once per shell:
  ```bash
  . ~/esp/esp-idf-v6/export.sh
  ```
  (v5.5.x also builds, but SBC-only — AAC needs v6+.)
- The **`espressif/esp_audio_codec`** managed component (fetched automatically by
  the IDF component manager on first `reconfigure`/`build`).

## Build

```bash
. ~/esp/esp-idf-v6/export.sh       # once per shell
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

## Building for AAC (ESP-IDF v6+)

AAC gives better quality than SBC from the iPhone. On **ESP-IDF v6+** (which you
already installed above) it's just two extra build flags — the AAC decode path and
endpoint are already in the code. Verified on **ESP-IDF v6.2.0**.

> Note: Earlier `BTA_AV_OPEN_EVT::FAILED status: 3` failures on v6 (for **both**
> SBC and AAC) were an **app-side init-order bug**, not an upstream defect: the
> external-codec stream endpoints must be registered *after* `esp_a2d_sink_init()`
> finishes (from the `ESP_A2D_PROF_STATE_EVT` init-success event); registering
> them synchronously right after the asynchronous init call silently fails
> (`ESP_ERR_INVALID_STATE`), leaving the sink with an empty codec table. Fixed in
> `main/bt_av.c`.

1. Build with AAC enabled (separate build dir + sdkconfig so it never clashes with
   the plain SBC build):
   ```bash
   . ~/esp/esp-idf-v6/export.sh
   cd pi-bt-mojo
   idf.py -B build-aac -DSDKCONFIG=build-aac/sdkconfig \
     -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.aac" \
     -DMOJO_ENABLE_AAC=1 set-target esp32
   idf.py -B build-aac -DSDKCONFIG=build-aac/sdkconfig build
   ```
   AVRCP (volume/metadata logging) stays enabled by default and works with AAC.
   Add `-DMOJO_ENABLE_AVRCP=0` if you want the smallest build without it.

2. Flash:
   ```bash
   idf.py -B build-aac -DSDKCONFIG=build-aac/sdkconfig -p /dev/cu.YOURPORT flash monitor
   ```

The iPhone will then negotiate AAC. Confirm in the log:
```
bt_av: registered AAC endpoint ...
bt_av: SEP register success, seid 0
bt_av: codec configured: AAC ...
render: opened AAC decoder (44100 Hz, 2 ch)
```
Everything downstream (drift buffer, S/PDIF, Mojo) is identical to the SBC path.
To go back to SBC, just build normally (no AAC flags).

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

## Troubleshooting

- **Boot loop with `E BOD: Brownout detector was triggered`** (reset right after
  `phy_init` / Bluetooth radio power-up): this is a **power** issue, not a firmware
  bug. The BT radio's current spike (largest during "full calibration") sags the
  3.3 V rail below the brownout threshold. Fixes, in order: use a short good-quality
  **data** USB cable; plug **directly** into the computer (no hub) or a powered hub;
  power via **5V/VIN** from a solid supply; keep peripherals off the 3V3 pin until
  it boots. Workaround if needed: `idf.py menuconfig` → *Component config → ESP
  System Settings → Brownout detector* → lower the level or disable it (bench-test
  only; fix the power too). Booting successfully once caches RF calibration in NVS,
  reducing later boot current.
- **Flash won't start / "Connecting…" fails:** hold the board's **BOOT** button
  during connect; ensure you selected the right `/dev/cu.*` port and installed the
  USB-UART driver (CP210x or CH34x).
- **Mojo won't lock to S/PDIF:** rebuild with `idf.py build -DSPDIF_SWAP_WORDS=1`
  (ESP32 32-bit I2S half-word-swap quirk); check the coax attenuator / TOSLINK wiring.
- **iPhone connects then drops** with `BTA_AV_OPEN_EVT::FAILED status: 3`
  (`BTA_AV_FAIL_STREAM`), with `Can't parse src cap` / `bta_av_open_failed` in the
  trace: the sink advertised **no codec endpoint**. This was an init-order bug —
  endpoints are now registered from the `ESP_A2D_PROF_STATE_EVT` init-success
  handler (fixed in `bt_av.c`). If you see a `SEP register FAILED` log, the
  endpoints didn't register; make sure you're running a build that includes this
  fix. (Advertising AAC on an IDF without `CONFIG_BT_A2DP_CODEC_AAC_ENABLED` is a
  separate cause — the firmware defaults to SBC only to avoid it.)

## Status & limitations

- **Codec:** builds on **ESP-IDF v6+**. **SBC by default**; add
  `-DMOJO_ENABLE_AAC=1` (+ the `sdkconfig.defaults.aac` overlay) to also offer
  **AAC**, which the iPhone prefers (see [Building for AAC](#building-for-aac-esp-idf-v6)).
  Full AAC A2DP-*sink* negotiation is gated by `CONFIG_BT_A2DP_CODEC_AAC_ENABLED`
  (v6+ only); on older v5.5.x the firmware still builds SBC-only. Both codecs
  decode via `esp_audio_codec`. aptX/LDAC are out of scope (iPhone never uses them).
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
