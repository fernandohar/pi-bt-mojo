# Wiring, BOM and hardware notes (ESP32-WROOM)

The ESP32 outputs a **3.3 V logic-level S/PDIF** bitstream on one GPIO
(`SPDIF_GPIO`, default **GPIO27**), generated in software from the I2S
peripheral. The Chord Mojo accepts that signal on either its **optical
(TOSLINK)** or **3.5 mm coax** S/PDIF input. Pick one path below.

Only 44.1/48 kHz is ever needed (A2DP's max), well within both Mojo inputs
(optical up to 192 kHz, coax up to 384 kHz).

> Note: `GPIO25`/`GPIO26` are used internally by the I2S peripheral as
> BCLK/WS. They are **not** part of the S/PDIF signal (leave them
> unconnected); only `GPIO27` (DOUT) carries S/PDIF.

## Software vs hardware S/PDIF — we use **software** (recommended)

This firmware generates S/PDIF in software (I2S biphase-mark). Rationale:

- The **Mojo strongly rejects jitter** (it reclocks its S/PDIF input), so the
  moderate jitter of software S/PDIF is largely masked — it is the ideal target
  for this approach.
- **Zero extra parts** beyond the tiny coax attenuator / TOSLINK module.

A **hardware transmitter** (e.g. WM8804) gives lower jitter and spec-perfect
levels but adds a chip + MCLK wiring; it's documented below as an optional
upgrade, not required for v1.

---

## Option A - Optical / TOSLINK (simplest, isolated)

```
 ESP32                            TOSLINK TX module            Chord Mojo
 ┌──────────────┐               ┌───────────────┐             optical in
 │ GPIO27 (SPDIF)●──────────────● DATA          │
 │  3V3     ●────────────────────● VCC (3.3 V)   ├───◎ ))) optical ─▶ ◎
 │  GND     ●────────────────────● GND           │
 └──────────────┘               └───────────────┘
```

| # | Part | Value / Spec | Notes |
|---|------|--------------|-------|
| 1 | TOSLINK transmitter | Toshiba TOTX147 / TOTX173 (or module) | 3.3 V logic data input |
| 2 | Decoupling cap | 0.1 µF across TX VCC/GND | close to the module |
| 3 | Optical cable | TOSLINK | plus a mini-TOSLINK adapter if your Mojo needs it |

---

## Option B - Coax 3.5 mm (cheapest, lowest jitter)

The coax input expects **~0.5 Vpp into 75 Ω**. A two-resistor series/shunt
network attenuates the 3.3 V logic swing and sets the source impedance near 75 Ω.

```
  ESP32                                                3.5 mm plug     Chord Mojo
                        Rs = 220 Ω                     (mono TS)       coax S/PDIF in
 ┌──────────────┐        ______
 │ GPIO27(SPDIF)●─────┬──|______|──┬───[ Cb ]───●===========● Tip ●────▶ SIGNAL
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

| # | Part | Value / Spec | Notes |
|---|------|--------------|-------|
| 1 | Resistor `Rs` | 220 Ω, 1% metal film | series, short leads at the GPIO |
| 2 | Resistor `Rp` | 100 Ω, 1% metal film | shunt to ground |
| 3 | Capacitor `Cb` (optional) | 0.1 µF C0G/film | DC block |
| 4 | 3.5 mm plug | mono TS (tip=signal, sleeve=gnd) | TRS ok, leave ring unconnected |
| 5 | Coax cable | 75 Ω (e.g. RG-179), < ~1.5 m | or a ready 75 Ω digital cable |

---

## Optional upgrade - hardware I2S→S/PDIF transmitter (WM8804)

For the lowest jitter, drive a dedicated transmitter instead of the software
encoder. This needs firmware changes (output standard I2S, not BMC) and is a
future option; the electrical hookup is:

```
 ESP32 (standard I2S)          WM8804                  Chord Mojo
 ┌──────────────┐        ┌──────────────────┐
 │ BCLK    ●─────┼────────● BCLK             │  coax  ●──────▶ coax in
 │ LRCLK   ●─────┼────────● LRCLK   TX out ──┼──(transformer)─▶
 │ DOUT    ●─────┼────────● DIN              │  optical (built-in) ─▶ optical in
 │ MCLK    ●─────┼────────● MCLK (if needed) │
 │ GND/3V3 ●─────┼────────● GND/3V3          │
 └──────────────┘        └──────────────────┘
```

| Part | Notes |
|------|-------|
| WM8804 (or DIT4192 / CS8406) | I2S-to-S/PDIF transmitter; needs MCLK from the ESP32 |
| S/PDIF output transformer + coax network, or the module's optical out | per the transmitter's datasheet |

To use it, replace [`spdif_out.c`](../main/spdif_out.c) with a standard I2S
output (16/24-bit, LRCLK at fs, MCLK = 256·fs) feeding the transmitter's DIN.
The rest of the pipeline is unchanged.

---

## Power

USB power to the ESP32 dev board works for a desktop bridge. The Mojo has its
own battery, so only the ESP32 needs power. For a portable build, add a LiPo +
charger/boost to the ESP32's 5V/3V3 rail.
