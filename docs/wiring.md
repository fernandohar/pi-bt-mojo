# Wiring, BOM and hardware notes

The Pico 2W outputs a **3.3 V logic-level S/PDIF** signal on one GPIO
(`SPDIF_PIN`, default **GPIO18**). The Chord Mojo accepts that signal on either
its **optical (TOSLINK)** or **3.5 mm coax** S/PDIF input. Pick one path below.

Only 44.1/48 kHz is ever needed (A2DP's max), so both Mojo inputs (optical up to
192 kHz, coax up to 384 kHz) have ample headroom.

---

## Option A — Optical / TOSLINK (simplest, galvanically isolated)

Drive a TOSLINK transmitter module from the S/PDIF GPIO.

```
 Pico 2W                         TOSLINK TX module            Chord Mojo
 ┌──────────────┐               ┌───────────────┐            optical in
 │ GPIO18 (SPDIF)●──────────────● DATA          │
 │  3V3     ●────────────────────● VCC (3.3 V)   ├───◎ ))) optical ─▶ ◎
 │  GND     ●────────────────────● GND           │
 └──────────────┘               └───────────────┘
```

BOM:

| # | Part | Value / Spec | Notes |
|---|------|--------------|-------|
| 1 | TOSLINK transmitter | Toshiba TOTX147 / TOTX173 (or module) | 3.3 V logic data input |
| 2 | Decoupling cap | 0.1 µF across TX VCC/GND | close to the module |
| 3 | Optical cable | TOSLINK | plus a mini-TOSLINK adapter if your Mojo needs it |

Pros: no impedance matching, ground-loop-proof. Cons: TX module cost, fragile
connector, slightly higher jitter than coax.

---

## Option B — Coax 3.5 mm (cheapest, lower jitter)

The coax input expects **~0.5 Vpp into 75 Ω**. A two-resistor series/shunt
network attenuates the 3.3 V logic swing and sets the source impedance near 75 Ω.

```
  Pico 2W                                              3.5 mm plug     Chord Mojo
                        Rs = 220 Ω                     (mono TS)       coax S/PDIF in
 ┌──────────────┐        ______
 │ GPIO18 (SPDIF)●────┬──|______|──┬───[ Cb ]───●===========● Tip ●────▶ SIGNAL
 │              │     │            │        center of 75Ω coax               (75 Ω
 │              │   (drive)     __│___                                        term
 │              │              |      | Rp = 100 Ω                            inside)
 │              │              |______|
 │              │                 │
 │  GND     ●───┴─────────────────┴──────────────────────────● Sleeve ●──▶ GROUND
 └──────────────┘                              shield of 75Ω coax

  Cb = OPTIONAL 0.1 µF DC-block in series with the center conductor
```

Design math (Vcc = 3.3 V, Zout = Rs∥Rp ≈ 75 Ω, Vload = Vcc·Rp/(Rs+Rp)·75/(75+Zout)):

| Rs (series) | Rp (shunt) | Zout | Vpp at Mojo | Notes |
|-------------|-----------|------|-------------|-------|
| 220 Ω | 100 Ω | 68.8 Ω | ~0.54 V | recommended (common values) |
| 240 Ω | 110 Ω | 75.4 Ω | ~0.52 V | closest to ideal |
| 330 Ω | 100 Ω | 76.7 Ω | ~0.38 V | good match, a bit low but still locks |

BOM:

| # | Part | Value / Spec | Notes |
|---|------|--------------|-------|
| 1 | Resistor `Rs` | 220 Ω, 1% metal film | series, keep leads short at the GPIO |
| 2 | Resistor `Rp` | 100 Ω, 1% metal film | shunt to ground |
| 3 | Capacitor `Cb` (optional) | 0.1 µF C0G/film | DC block |
| 4 | 3.5 mm plug | mono TS (tip=signal, sleeve=gnd) | TRS ok, leave ring unconnected |
| 5 | Coax cable | 75 Ω (e.g. RG-179), < ~1.5 m | or a ready 75 Ω digital cable |

Pros: cheap, sturdy, lowest jitter. Cons: no isolation (not an issue here — the
Mojo is battery powered and the Pico is low-power, so there is no mains ground
loop).

---

## I2S bring-up (optional, for testing without the Mojo)

Build with `-DAUDIO_OUTPUT=i2s` and wire a PCM5102 board:

| PCM5102 | Pico 2W GPIO |
|---------|--------------|
| DIN     | `I2S_DATA_PIN` (default GPIO9) |
| BCK     | `I2S_CLOCK_PIN_BASE` (default GPIO10) |
| LRCK    | `I2S_CLOCK_PIN_BASE`+1 (default GPIO11) |
| GND/VCC | GND / 3V3 |

---

## Power (optional, for a portable unit)

The Mojo has its own battery, so only the Pico needs power. For a portable
build, add a LiPo + charger/boost (e.g. a Pico-compatible LiPo shim) to the
Pico's VSYS/GND. USB power works for a desktop bridge.

---

## Optional / future: AAC codec

v1 uses **SBC** (built into BTstack; the iPhone falls back to SBC automatically).
Better quality is possible with **AAC** via Fraunhofer FDK-AAC:

- Add the FDK-AAC decoder (note licensing terms) and an AAC A2DP endpoint
  alongside the SBC endpoint in [`a2dp_sink.c`](../firmware/a2dp_sink.c).
- Re-check CPU headroom on the RP2350 — AAC decode is heavier than SBC. The
  current SBC build leaves plenty of margin (text ≈ 444 KB, BSS ≈ 38 KB), but
  AAC decode load should be profiled before committing to it.

Since the audio is already lossy over Bluetooth, SBC at high bitpool is a
reasonable v1; AAC is a quality upgrade, not a correctness fix.
