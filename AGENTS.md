# AGENTS

## Cursor Cloud specific instructions

This repo is firmware for a **Raspberry Pi Pico 2W** (RP2350) — a wireless
iPhone→Chord Mojo audio bridge. It cross-compiles to a `.uf2`; it does **not**
run on the VM. There is one buildable component (the `mojo_bt_bridge` firmware).

### Environment (handled by the update script)
- ARM toolchain: `gcc-arm-none-eabi` + newlib.
- Host tools for `picotool`/`pioasm`: `build-essential`/`g++` + `libusb-1.0-0-dev`.
  Gotcha: `/usr/bin/c++` is **gcc-14**, so the matching `libstdc++-14-dev` must be
  installed or the SDK's `picotool`/`pioasm` sub-builds fail with
  `cannot find -lstdc++` (even though `g++`→gcc-13 links fine standalone).
- Pico SDK + pico-extras are cloned to `~/pico/pico-sdk` and `~/pico/pico-extras`
  (SDK ≥ 2.0 required for RP2350). The top-level `CMakeLists.txt` auto-detects
  these paths, so **no `PICO_SDK_PATH` env var is needed**.

### Build / lint / test
- Build (S/PDIF, default): `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4`
  → `build/firmware/mojo_bt_bridge.uf2`.
- I2S bring-up variant: add `-DAUDIO_OUTPUT=i2s` (use a separate build dir).
- There is no separate lint/test suite; the compiler (with warnings) is the
  gate. Treat a clean `cmake --build` for both `AUDIO_OUTPUT=spdif` and `=i2s`
  as the "build passes" check.
- First `cmake --build` after a fresh checkout is slow: it builds `picotool`
  from source (needs network + host g++/libusb). Subsequent builds are fast.

### Non-obvious gotchas
- Do **not** name a local header `a2dp_sink.h` with guard `A2DP_SINK_H` — it
  collides with BTstack's `classic/a2dp_sink.h`. Our header uses guard
  `BRIDGE_A2DP_SINK_H`. Likewise avoid the type names `avrcp_connection_t` /
  other BTstack public typedefs for local structs.
- RP2350 + `pico_audio` requires explicit `SPINLOCK_ID_AUDIO_FREE_LIST_LOCK` and
  `SPINLOCK_ID_AUDIO_PREPARED_LISTS_LOCK` defines (set in `firmware/CMakeLists.txt`)
  or the build hits an `#error` in `pico/audio.h`.
- BT here is **classic only** (A2DP/AVRCP). We link `pico_btstack_classic` +
  `pico_btstack_cyw43` and set `CYW43_ENABLE_BLUETOOTH=1`, `CYW43_LWIP=0` (no
  networking → no `lwipopts.h` needed).

### Hardware-in-the-loop (cannot be done on the VM)
Pairing a real iPhone and confirming Mojo lock/audio needs physical hardware
(a Pico 2W, the S/PDIF wiring in `docs/wiring.md`, and a Mojo). The VM can only
verify that the firmware compiles to a valid RP2350 image. See `README.md`
"Use / verify" for the on-device procedure.
