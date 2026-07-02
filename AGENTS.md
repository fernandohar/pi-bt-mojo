# AGENTS

## Cursor Cloud specific instructions

This repo is **ESP-IDF firmware for an ESP32-WROOM** — a wireless iPhone→Chord
Mojo audio bridge (A2DP AAC in, software S/PDIF out). It cross-compiles to a
`.bin`; it does **not** run on the VM. There is one buildable app (`mojo_bt_bridge`).

### Environment (handled by the update script)
- **ESP-IDF v5.5.1** is cloned to `~/esp/esp-idf` and its esp32 toolchain is
  installed via `install.sh esp32`.
- `~/.bashrc` auto-sources `~/esp/esp-idf/export.sh`, so new interactive shells
  have `idf.py`. In a non-interactive shell, run `. ~/esp/esp-idf/export.sh` first.
- The **`espressif/esp_audio_codec`** managed component (AAC + SBC decoders) is
  fetched by the IDF component manager on the first `idf.py reconfigure`/`build`
  (needs internet the first time; cached afterwards in `managed_components/`).

### Build / "test"
- `idf.py set-target esp32` (once), then `idf.py build` → `build/mojo_bt_bridge.bin`.
- There is no automated test suite; a **clean `idf.py build` is the gate**.
- `sdkconfig` is generated from `sdkconfig.defaults` and is gitignored; if you
  change `sdkconfig.defaults`, run `rm sdkconfig && idf.py set-target esp32`.

### Non-obvious gotchas
- **Do NOT background long commands with `&` in the persistent Shell** — a
  lingering child holds the tool's output pipe open and **jams the shell** for the
  rest of the session (symptom: every command returns "no exit status"). Recover
  by passing a `working_directory` to spawn a fresh shell. Use a **foreground
  call with a long timeout**, or tmux, for the ESP-IDF install/build.
- ESP-IDF's `install.sh` needs **`python3-venv`** (`ensurepip`); without it the
  Python env creation fails. The update script installs it.
- Bluetooth is **classic BR/EDR only** (`CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY`,
  `CONFIG_BT_BLE_ENABLED=n`) with the **external-codec A2DP sink**
  (`CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y`). We register an AAC (`M24`) endpoint
  (primary, iPhone) + SBC (fallback) and decode frames ourselves via
  `esp_audio_codec`. v5.5.1 has no `BT_A2DP_CODEC_AAC_ENABLED` (master-only); the
  external-codec path is codec-agnostic, so we supply the AAC capability + decoder.

### Needs real hardware (cannot be verified on the VM)
- Pairing a real iPhone + confirming Mojo lock/audio.
- **S/PDIF bit/word ordering**: the software encoder in `main/spdif_out.c` follows
  the S/PDIF spec, but the ESP32 32-bit I2S peripheral may half-word-swap; if a
  scope/DAC shows a swapped stream, set `SPDIF_SWAP_WORDS=1`.
- **AAC framing**: A2DP AAC is decoded as raw AAC-LC (no ADTS) at 44.1 kHz stereo
  (the iPhone case). Other sources/rates need the `M24` codec-capability element
  parsed in `main/bt_av.c`.
