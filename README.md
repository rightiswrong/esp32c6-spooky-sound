# 🎃👂 Spooky Sound — a Zigbee Halloween light that listens

A **Seeed XIAO ESP32-C6** on the **XIAO Expansion Board** drives a string of
**Grove Chainable RGB LED v2.0** modules with the ten Halloween effects of
[NanoC6 Spooky Lights](https://github.com/rightiswrong/m5nanoc6-zigbee-lighting-control),
and listens through a **Grove Analog Microphone**:

* a thunderclap at the speaker **strikes Lightning Storm about 21 ms later** (measured in simulation);
* every crackle of a fireplace track **flares one candle white-hot**;
* a heartbeat sets the **Tell-Tale Heart**'s tempo; a scream sets off the **Poltergeist**.

A companion **Home Assistant integration (HACS)** connects it to
**Music Assistant**. It switches effects to match the track that's playing and, with
**Sendspin**, sends cues timed to the music itself.

[![HACS Custom](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://hacs.xyz/)
[![Validate](https://github.com/rightiswrong/esp32c6-spooky-sound/actions/workflows/validate.yml/badge.svg)](https://github.com/rightiswrong/esp32c6-spooky-sound/actions/workflows/validate.yml)
[![Release](https://img.shields.io/github/v/release/rightiswrong/esp32c6-spooky-sound)](https://github.com/rightiswrong/esp32c6-spooky-sound/releases/latest)

> **It enhances Spooky Lights; it doesn't depend on it.** Zigbee endpoints 10–15 are
> identical to the NanoC6's, so the Spooky Lights integration drives this light
> unchanged (named effects, spotlight, jump scare). Spooky Sound adds endpoints
> 16–20 and works with or without Spooky Lights installed. It can even make a
> mic-less NanoC6 follow your soundtrack.

---

## What's in the box

| Path | What |
|---|---|
| `firmware/XIAO_SpookySound/` | Arduino sketch for the XIAO ESP32-C6 (Zigbee **router**) |
| `custom_components/spooky_sound/` | Home Assistant integration (installed by HACS) |
| `zigbee2mqtt/xiao_spooky_sound.mjs` | Optional external converter if you use Zigbee2MQTT instead of ZHA |
| `docs/WIRING.md` | Ports, power, the loopback for LED hot-plug detection, mic placement |
| `docs/MUSIC_ASSISTANT.md` | Echo Show / View Assist / Sendspin, and timing the lights to the music |
| `docs/HOW_IT_HEARS.md` | The signal processing, step by step, and how it was tuned |
| `tools/hosttest/` | Firmware + DSP compiled on a PC: P9813 chain simulator, synthetic thunder / fire / music / screams |
| `tests/`, `tests_ha/` | Integration logic & firmware-sync tests; integration tests inside a real Home Assistant |

## Hardware

| Part | Notes |
|---|---|
| Seeed Studio XIAO ESP32-C6 | Zigbee + the brains |
| Seeed Studio Expansion Board Base for XIAO | Grove ports, OLED (shows a live "seismograph"), button |
| Grove – Analog Microphone | on the **A0/D0** Grove port |
| Grove – Chainable RGB LED v2.0 (×1…24) | on the **UART** Grove port |
| 5 V / 2 A USB-C supply | the LEDs want 5 V — see [WIRING](docs/WIRING.md) |
| optional: 10 kΩ + 18 kΩ resistors, a return wire | live LED counting when you plug modules in and out |

## The effects and what they hear

| # | Effect | Without sound | With sound |
|---|---|---|---|
| 0 | Solid | Your colour, steady | — |
| 1 | Candle Flicker | Each pixel its own flame | Flames rise and fall with the fire's loudness; **each crackle flares a flame white-hot**, sometimes spitting a spark to its neighbour; a boom makes every flame gutter |
| 2 | Ghost Breath | A breath rolling down the string | Brightens with the room |
| 3 | Lightning Storm | Random strikes every 3–9 s | **A thunderclap strikes immediately**, sized by its loudness; rumble makes the clouds glow; while the soundtrack thunders, the storm stops inventing strikes |
| 4 | Witch's Cauldron | Bubbles swell and pop | Crackles and booms burst bubbles; noise brings it to a rolling boil |
| 5 | Tell-Tale Heart | lub-dub at 55 BPM | **Locks to a heard heartbeat** (beat cues or bassy thumps) |
| 6 | Haunted Chase | A prowling eye | A boom startles it: it whips round and bolts |
| 7 | Poltergeist | Random lurid throws | Booms throw pixels; **a scream grabs the power** |
| 8 | Possessed Pumpkin | Candle that turns sickly green | As Candle, plus the possession |
| 9 | Blood Moon | Crimson tide | — |

"Sound" means the **microphone**, **Home Assistant cues**, or both: see *Sound source*
below. With silence, every effect behaves exactly as in Spooky Lights.

---

## Quick start

### 1. Wire it
Microphone → **A0/D0** port. LED string → **UART** port. Feed the string's 5 V from
the expansion board's 5 V pin (details and a picture-free walkthrough in
[`docs/WIRING.md`](docs/WIRING.md)). Before plugging the microphone in, check the
Grove red wire with a meter (3.3 V is what the mic wants).

### 2. Flash the firmware

**Option A — prebuilt (fastest).** Download `xiao-spooky-sound-<version>-factory.bin`
from the [latest release](https://github.com/rightiswrong/esp32c6-spooky-sound/releases/latest),
open [ESPHome Web](https://web.esphome.io) in Chrome or Edge, connect the XIAO over USB-C,
choose **Install** and pick the file (hold **BOOT** while plugging in if it won't connect).

**Option B — Arduino IDE 2.x.** Boards Manager → **esp32 by Espressif Systems ≥ 3.3.0**, open
`firmware/XIAO_SpookySound/XIAO_SpookySound.ino`, then Tools:
**Board: XIAO_ESP32C6** · **USB CDC On Boot: Enabled** ·
**Partition Scheme: Zigbee ZCZR 4MB with spiffs** · **Zigbee mode: Zigbee ZCZR (coordinator/router)**.
Upload; Serial Monitor at 115200 shows the mic levels and LED count.

### 3. Pair it (ZHA)
Settings → Devices & services → Zigbee Home Automation → **Add device**, then power the XIAO.
It appears as **SpookyLab XIAO-C6-SpookySound**: a light, two sensors (Grove LEDs, sound level,
sound event) and number controls. *Re-pairing resets Zigbee:* hold the button **5 s**,
the LEDs turn red, release.

*Zigbee2MQTT?* Copy `zigbee2mqtt/xiao_spooky_sound.mjs` into `<z2m data>/external_converters/`
and restart Z2M before pairing.

### 4. Install the integrations (HACS)
HACS → ⋮ → **Custom repositories** →

* `https://github.com/rightiswrong/esp32c6-spooky-sound` (type **Integration**) — **Spooky Sound**
* optionally `https://github.com/rightiswrong/m5nanoc6-zigbee-lighting-control` — **Spooky Lights**
  (named effects, spotlight, jump-scare button — works on this light as-is)

Restart Home Assistant, then **Add integration → Spooky Sound**: pick the light, check the
auto-mapped entities, and optionally pick your Music Assistant player.

[![Open in HACS](https://my.home-assistant.io/badges/hacs_repository.svg)](https://my.home-assistant.io/redirect/hacs_repository/?owner=rightiswrong&repository=esp32c6-spooky-sound&category=integration)

---

## What you get in Home Assistant

| Entity | Purpose |
|---|---|
| **Sound source** (select) | Off · Microphone · Home Assistant / Music Assistant · Both |
| **Sound heard** (event) | `thunder` · `fire` · `loud` (a scream) — as the light's microphone hears it |
| **Soundtrack mood** (sensor) | none · storm · fire · heartbeat · cauldron · haunting, from the followed player's track |
| **Follow the soundtrack** (switch) | A storm track switches the light to Lightning Storm, a fireplace to Candle Flicker… and back when it stops |
| **Sync lights to the music** (switch) | Sendspin cues on/off |
| **Light lead time** (number) | How early cues are sent so the flash lands on the sound |
| **Sendspin** (diagnostic sensor) | Bridge status, frames received, cues sent / dropped |
| **Test thunder** (button) | One lightning strike, now |
| Mic sensitivity, Sound level, … | Raw firmware controls from ZHA / Z2M |

Plus the action **`spooky_sound.send_cue`** (thunder · rumble · fire · crackle · beat · peak · clear)
and the bus event `spooky_sound_detected`.

### Example automations

```yaml
# Doorbell → a thunderclap on the porch lights
- alias: Trick-or-treat thunder
  triggers:
    - trigger: state
      entity_id: binary_sensor.front_doorbell
      to: "on"
  actions:
    - action: spooky_sound.send_cue
      data: { cue: thunder, strength: 1 }

# Someone screamed at the door — tell me
- alias: A scream on the porch
  triggers:
    - trigger: state
      entity_id: event.xiao_spooky_sound_heard
      attribute: event_type
      to: loud
  actions:
    - action: notify.mobile_app_phone
      data: { message: "👻 Got one!" }
```

(Entity IDs depend on your device name — check Settings → Entities.)

---

## Music Assistant (your Echo Show)

There are two ways in. Both are described in full in [`docs/MUSIC_ASSISTANT.md`](docs/MUSIC_ASSISTANT.md).

1. **Metadata. Works with any player, including View Assist on an Echo Show.** The integration
   reads the track's title, artist and album, and switches the effect to match. The **microphone**
   then hears the speaker and does the per-thunderclap timing locally.
2. **Sendspin. Sample-accurate, but the speaker must be a Sendspin player.** The integration joins
   Music Assistant as a *visualizer*. MA streams it each peak, beat and spectrum frame, stamped
   with the moment its audio plays, ahead of the audio. Cues go out early by the light's Zigbee
   delay, so the strike lands on the clap. On an Echo Show this means running the **SendspinDroid**
   app, since View Assist doesn't speak Sendspin yet.

---

## How it works (short version)

* **Listening.** The microphone is sampled by DMA at 32 kHz and decimated to 16 kHz. It is split
  into low (< 300 Hz), mid (> 300 Hz) and high (> 2 kHz) bands with fixed-point filters (the C6
  has no FPU). Each 8 ms frame is judged against the running mean of the recent past. Onsets
  become **hits**. A hit with *body* (still building 16–24 ms in) is a **boom**, while a 2–4 ms
  click is a **crackle**. Booms are classified 0.3 s later as **thunder** (noise that rolls on) or a
  **scream** (periodic). Hit to light is ~21 ms. The full story, with the dead ends, is in
  [`docs/HOW_IT_HEARS.md`](docs/HOW_IT_HEARS.md).

  ![What the analyser hears](docs/img/how-it-hears.png)

* **Zigbee.** Everything Spooky Lights had, plus: sound source (16), mic sensitivity (17), a cue
  channel (18: `seq·10000 + type·1000 + param`, exact in a float32), sound level (19) and sound
  event (20: `seq·10 + type`). All standard ZCL clusters, so ZHA needs no quirk.
* **Threads.** The mic runs in its own FreeRTOS task (above `loop()`, below Zigbee). A `SoundBus`
  collects mic and remote events under a spinlock, and the renderer takes one snapshot per 20 ms
  frame, so no crackle is lost between frames.
* **OLED.** A tiny SSD1306 driver pushes one 8-row page per frame (~3 ms of I²C). It shows the
  effect, a 5-second seismograph of what the mic hears (full-height marks for strikes), the
  sound source, and the latest event. Use it to set the mic sensitivity by eye.

---

## Testing

```bash
tools/hosttest/run.sh                   # firmware + DSP on a PC (needs g++); --trace DIR for CSVs
python3 -m unittest discover -s tests   # integration logic + firmware ⇄ integration sync checks
pip install pytest-homeassistant-custom-component && python3 -m pytest   # integration in HA
python3 tools/plot_traces.py DIR        # redraw docs/img/how-it-hears.png from --trace output
```

The host test compiles the real sketch against API stubs, a bit-level P9813 chain model, an
SSD1306 panel model and a synthetic sound generator. It covers:

* thunder in a room and through a small speaker, a log fire, music at several tempos, and screams;
* a robustness sweep of 69 scenarios across loudness (up to ADC clipping), speakers and random seeds;
* mic→light latency, OLED page streaming, the power limiter, buttons, persistence, and the Zigbee endpoints;
* builds under AddressSanitizer/UBSan, for both versions of the Arduino Zigbee colour-light API.

## Status: what has and hasn't been verified

* ✅ Verified in simulation: every check above passes. GitHub Actions compiles the sketch for the
  XIAO ESP32-C6 with Espressif's core and runs hassfest, HACS validation and the HA tests.
* ⚠️ **Not yet run on real hardware.** The DSP thresholds are tuned on synthetic sounds. Real
  microphones, rooms and the Echo Show's speaker will differ, and the *Mic sensitivity* control
  and the OLED meter are there to close that gap. Expect the first evening to need a tweak or two.
* ⚠️ The Sendspin bridge follows `aiosendspin` 10.x and Music Assistant 2.10's visualizer
  protocol. Both move quickly; if they change, the bridge reports an error and the rest keeps
  working.

## License

GPL-3.0 — see [LICENSE](LICENSE). The effects engine, renderer and P9813 driver come from
NanoC6 Spooky Lights (MIT, same author). The OLED font is the classic Adafruit GFX 5×7
(BSD, notice in `firmware/XIAO_SpookySound/font5x7.h`).
