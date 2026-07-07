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

### Codec status (important)
Full **AAC A2DP-sink** stream negotiation only exists on **ESP-IDF v6 / `master`**
(`CONFIG_BT_A2DP_CODEC_AAC_ENABLED`), NOT on v5.5.1. On stable IDF, advertising
AAC makes the source pick it and the stream open fails
(`BTA_AV_OPEN_EVT::FAILED status: 3` / `BTA_AV_FAIL_STREAM`, often preceded by
`BT_AVCT: Out of ccbs`). So `main/bt_av.c` advertises **SBC only** by default;
`-DMOJO_ENABLE_AAC=1` + `sdkconfig.defaults.aac` adds the AAC endpoint on a v6+
toolchain. Both codecs decode via `esp_audio_codec`. The A2DP external-codec API
is identical between v5.5.1 and v6, so no source changes are needed for AAC — only
the newer toolchain + build flags (see README "Building for AAC"). Verified
building on ESP-IDF v6.2.0.

### v6 A2DP sink: register endpoints AFTER init (RESOLVED - was NOT an upstream bug)
Earlier `BTA_AV_OPEN_EVT::FAILED status: 3` failures on v6 (for BOTH SBC and AAC,
also reproduced with Espressif's `a2dp_sink_stream_aac` example) were an
**app-side init-order race**, not a stack regression. `esp_a2d_sink_init()` is
asynchronous; the stack's `g_a2dp_on_deinit` flag stays `true` and the A2DP state
machine is not yet `IDLE` until the queued init message is processed. Calling
`esp_a2d_sink_register_stream_endpoint()` synchronously right after init makes it
return `ESP_ERR_INVALID_STATE` and **never enqueue** the registration, so no codec
endpoint is ever created. `bta_av_co_audio_peer_src_supports_codec()` then finds an
empty `codec_caps` table (`id=0xff`) and returns FALSE -> `bta_av_open_failed`.

Fix (in `main/bt_av.c`): register the stream endpoints + audio-data callback from
the `ESP_A2D_PROF_STATE_EVT` init-success handler, not at stack-up. A `SEP register
FAILED` log means registration was rejected (still too early). The stock examples
happen to win this race; our AVRCP init + Core-1 render task made us lose it.

The `Can't parse src cap ret = 13` (A2D_WRONG_CODEC) trace line is unrelated
non-fatal noise (also fires at boot) - do not chase it. With the fix, AAC on v6
(`-DMOJO_ENABLE_AAC=1` + `sdkconfig.defaults.aac`) should negotiate normally.

### Two ESP-IDF versions on one machine (gotcha)
The SBC (default) build uses ESP-IDF **v5.5.1**; the AAC build uses **v6**. Do NOT
source both in one shell — their Python venvs conflict (`export.sh` fails with a
venv-mismatch error). Use a fresh shell per version, or unset
`IDF_PATH`/`IDF_PYTHON_ENV_PATH` before sourcing the other. Keep the two builds in
separate dirs with separate sdkconfigs (`-B build-aac -DSDKCONFIG=build-aac/sdkconfig`).
