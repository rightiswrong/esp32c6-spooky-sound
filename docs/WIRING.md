# Wiring

```
                       Seeed Expansion Board for XIAO
                  ┌──────────────────────────────────────┐
  Grove Analog    │ [A0/D0]   OLED 128×64    [I2C] [I2C] │
  Microphone ─────┤                                      │
                  │ [UART] ◄── Grove Chainable LEDs ───┐ │
                  │                                     │ │
                  │  5V ●  ◄── red wire of the LED chain┘ │  (recommended, see Power)
                  │  btn (D1)        XIAO ESP32-C6 + BOOT │
                  └──────────────────────────────────────┘
```

## 1. Ports and pins

| Grove port | Wire | XIAO pin | GPIO | Used for |
|---|---|---|---|---|
| **A0/D0** | yellow | A0 / D0 | 0 | Microphone signal → ADC1 channel 0 |
|  | white | A1 / D1 | 1 | (not used by the mic; shared with the board's button) |
| **UART** | yellow | D7 / RX | 17 | P9813 **CI** (clock) |
|  | white | D6 / TX | 16 | P9813 **DI** (data) + loopback sense |
| I2C ×2 | — | D4/D5 | 22/23 | Left free: the OLED and RTC live on this bus |

The pin numbers come from the Arduino core's XIAO ESP32-C6 variant. The UART port's colour
order follows the usual Grove convention (pin 1 = RX). **If your LEDs stay dark,** the port is
wired the other way round on your board: swap `PIN_GROVE_CLK` / `PIN_GROVE_DATA` in
`config.h`.

Why not an I²C port for the LEDs? The bit-banged LED data would be clocked into the OLED and
RTC that share that bus, and garble the display.

## 2. Power — read this before plugging in

* **The microphone wants 3.3 V.** It's a 3.3 V part, and its output must stay below 3.3 V
  because the ESP32-C6 is not 5 V tolerant. Measure the A0 port's red wire. The expansion
  board's Grove ports are generally 3.3 V, which is ideal, but if yours shows 5 V, power the mic
  from the 3V3 pin instead.
* **The LEDs want 5 V.** Grove Chainable RGB LED v2.0 is specified for 5 V, and at 3.3 V blue
  and white are weak. Make a short adapter: keep the UART port's **yellow, white and black**,
  but take the **red** from the expansion board's **5 V** pin (USB 5 V). Leave `POWER_BUDGET_MA`
  at 1000 with a 2 A supply. If you run the chain from the Grove VCC instead, drop it to ~350.
* The P9813's inputs are driven at 3.3 V logic. That's the same as the NanoC6 build, and fine in
  practice for these modules.
* Long strings (> ~15 modules): give the chain its own 5 V supply on the red wire, share GND,
  and never tie two 5 V supplies together.

## 3. Optional: live LED counting (loopback)

Exactly as in Spooky Lights, but on the UART port's **white** wire (GPIO16). Route the last
module's DO back through a divider:

```
GPIO16 (DI) ──┬─────────────────────────► module #1 DI
              ├── R2 ── GND
              └── R1 ── return wire ◄──── last module DO
```

| Chain powered from | R1 (series) | R2 (to GND) | Sense voltage |
|---|---|---|---|
| **5 V** (recommended) | 10 kΩ | 18 kΩ | 5 × 18/28 ≈ 3.2 V ✔ |
| 3.3 V | 10 kΩ | 100 kΩ | 3.3 × 100/110 ≈ 3.0 V ✔ (18 kΩ would give 2.1 V, below the C6's ~2.5 V "high") |

Without the return wire, set `CHAIN_DETECT_LOOPBACK 0` in `config.h` and the LED count from
Home Assistant ("Manual LED count").

A note on boot: while the ROM bootloader runs, GPIO16 is its serial TX, so the first ~0.5 s
may show random colours until the firmware's first frame. Harmless. If it bothers you, add a
100 kΩ pull-down on CI.

## 4. The microphone (optional)

Without one, leave the A0 port empty: the firmware detects that and follows Home Assistant /
Music Assistant cues only (see the README's *Without a microphone*). Don't leave a bare Grove
cable dangling from the port, since a long open wire picks up noise.

* **Placement.** Point it at the speaker, 0.5–3 m away, away from the LEDs' power wiring. For
  the Echo Show, beside or below the screen works well.
* **Calibration.** Watch the OLED seismograph and the `[mic]` serial line (every 10 s: raw
  ADC range, dBFS, level, rumble, crackle rate).
  * At rest, the raw range should hover around 2000–2200 with a spread of a few counts. A flat
    line at 0 or 4095 means it's unplugged or miswired.
  * Clap. You should see a full-height strike on the seismograph and a flash in Lightning Storm.
  * Too twitchy? Lower **Mic sensitivity** (Home Assistant, endpoint 17). Missing things? Raise it.
    Each ±16 doubles or halves every threshold.
* **Privacy.** Audio never leaves the XIAO. Only levels and three event types (thunder, fire,
  scream) are reported over Zigbee.

## 5. Buttons, LED, antenna

* **Button** (expansion board, or the XIAO's BOOT):
  * click: next effect
  * double-click: next sound source (the LEDs flash red / green / blue / violet for off / mic / remote / both)
  * hold 1–5 s: on/off
  * hold ≥ 5 s, then release: Zigbee factory reset
* If GPIO1 reads "pressed" all through boot (something holding the shared mic-port wire low), the
  firmware ignores it and uses BOOT only.
* **Orange user LED** stands in for the NanoC6's RGB LED: it shows the brightness of the
  effect's first pixel whenever the output includes "onboard". If it's lit when it should be
  dark, set `USER_LED_ACTIVE_LOW 0`.
* **Antenna.** The on-board ceramic antenna is used. Set `USE_EXTERNAL_ANTENNA 1` for a u.FL
  antenna, which helps if the porch is far from the mesh.
