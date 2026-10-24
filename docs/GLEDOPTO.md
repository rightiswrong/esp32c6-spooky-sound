# Spooky Overhead — the Halloween effects on a Gledopto GL-C-211WL

Hidden overhead lighting (a strip in a cove, above the cabinets, under the porch roof) that
plays the same ten Halloween effects as the XIAO / NanoC6 lights and **hears the room the same
way**: a thunderclap strikes Lightning Storm, a fireplace crackles the Candle Flicker.

The **Gledopto GL-C-211WL** is a classic ESP32 (Wi-Fi, no Zigbee radio) driving an analog
12–24 V strip with PWM. So this is not the XIAO firmware. It's **ESPHome firmware** built
from the same C++: the same effects engine and the same sound analyser, plus a component
(`esphome/components/spooky_show`) that turns them into an ordinary ESPHome light.

```
Grove mic ──► IO33 (ADC1, DMA 32 kHz) ──► SoundAnalyzer ──┐            (same code as the XIAO)
Home Assistant / Music Assistant cues ──► SoundBus ◄──────┘
                                              │ one snapshot per 20 ms frame
                                              ▼
                     Effects engine: 6 virtual "flames" ──► ceiling mixer ──► 1 colour
                                                                               │
                         R G B (+ warm, cold white) PWM ◄── channel split ◄────┘
```

---

## What has and hasn't been checked

| | |
|---|---|
| ✅ | Effects, analyser and mixer run on a PC in the host tests (`tools/hosttest/test_strip.cpp`): thunder through the mic flashes the strip in the next frame and Home Assistant hears **thunder**; a log fire flickers without strobing (biggest frame-to-frame jump < 0.6) and is heard as **fire**; with an empty mic pad, cues alone drive the show. |
| ✅ | GitHub Actions compiles all three variants with ESPHome 2026.10.0 on every push, and checks the image fits WLED's app slot. |
| ⚠️ | **Not run on the controller yet.** |
| ⚠️ | **Which GPIO drives which colour** is taken from the label order (V+, GPIO19, 18, 17, 16, 4). Confirm it from WLED **before** you flash (below), or with the *Channel test* button after. |
| ⚠️ | **The Function button's GPIO** is unknown. GPIO0 is a guess. If it's wrong the button does nothing; nothing else is affected. |
| ⚠️ | **Flashing from WLED over the air** works on many ESP32 WLED controllers, but not every vendor partition table. If it fails, nothing is lost: WLED keeps running, and the serial route below always works. |

---

## 1. Before you flash: read three things from WLED

The controller ships with WLED, which already knows its pins. Open its web page (WLED app, or
`http://<its-ip>`) and note:

1. **Config → LED Preferences.** The output type (RGB / RGBW / RGBCCT = "5 channel") and the
   GPIOs **in order**. For an analog output WLED lists them as *R, G, B, W(arm), C(old)*.
2. **Config → LED Preferences → Button 0 GPIO.** That's the Function button.
3. Your **strip**: count the wires besides V+. 3 = RGB, 4 = RGBW, 5 = RGB + warm + cold (RGBCCT).

Or in a browser: `http://<its-ip>/json/cfg` (look for `hw.led.ins[0].pin` and `hw.btn.ins[0].pin`)
and `http://<its-ip>/json/info` (`arch` should say `esp32`).

If the pins differ from the defaults (R 19, G 18, B 17, warm/W 16, cold 4, button 0), build
your own firmware with the right substitutions (section 3). The prebuilt files assume the
defaults.

**Save WLED's config first:** *Config → Security & Updates → Backup configuration*. Then you can
go back to WLED any time with the WLED `.bin` from its releases.

## 2. Wire it

| Terminal / pad | To |
|---|---|
| V+ | strip common + (12 or 24 V, whatever the strip needs) |
| GPIO19 / 18 / 17 | strip R / G / B (−) |
| GPIO16 | warm white (RGBCCT) or white (RGBW) |
| GPIO4 | cold white (RGBCCT only) |
| DC in | the strip's supply. Size it for the strip; the controller adds ~1 W |

**The microphone (optional).** Grove Analog Microphone:

| Grove wire | To |
|---|---|
| yellow (SIG) | **IO33** pad |
| black (GND) | **GND** pad |
| red (VCC) | **3.3 V** (see below) |
| white | not used |

The pads give you IO33 and GND but **no 3.3 V**. Two ways to power the mic:

* **Best:** a tiny 3.3 V buck from V+ (any 12–24 V-in, 3.3 V-out module rated for a few mA,
  e.g. a Pololu D24V5F3-class part), its ground to the GND pad.
* Or the controller's own 3.3 V rail: the ESP32 module's 3V3 pin or the regulator's output.
  Measure it with a meter before you solder.

**Never 5 V or more.** The mic idles at half its supply, so on 5 V it would swing up to 5 V,
above what the ESP32's ADC tolerates. Keep the mic lead short and away from the PWM wires.
Twisting the signal with its ground helps. The firmware samples IO33 on ADC1, which works
alongside Wi-Fi (ADC2 doesn't).

No mic? Leave IO33 empty. The firmware pulls the pad down, notices within a quarter of a
second that nothing's there, and runs on Home Assistant / Music Assistant cues alone.

## 3. Flash

Each release has two files per strip type: `spooky-overhead-<strip>-<version>-ota.bin` and
`…-factory.bin`, for `rgbcct`, `rgbw` or `rgb`.

### A. From WLED, over Wi-Fi (no cables)

1. WLED → **Config → Security & Updates → Manual OTA update**.
2. Upload `spooky-overhead-<strip>-<version>-ota.bin`.
3. The controller reboots as **Spooky Overhead**. Join the Wi-Fi hotspot
   **"Spooky Overhead setup"** from your phone, pick your network, and save.
4. Home Assistant discovers it (*Settings → Devices → ESPHome, Discovered*). Click **Add**.

If WLED refuses the file (some builds check the image), use B.

### B. Over serial (always works; also the recovery route)

You need a 3.3 V USB-serial adapter on the board's TX / RX / GND / IO0 pads, or the USB port if
your unit has one. Hold **IO0 to GND** (or the Function button if it is on GPIO0) while powering
up, then:

* <https://web.esphome.io> in Chrome or Edge → **Connect → Install →**
  `spooky-overhead-<strip>-<version>-factory.bin`, or
* `esptool --chip esp32 write_flash 0x0 spooky-overhead-<strip>-<version>-factory.bin`

Use the factory file at 0x0. It contains a partition table, so it replaces WLED's.

### C. Build your own (different pins, a fixed Wi-Fi, an API key)

In the ESPHome dashboard (Home Assistant add-on), **Take control** of the discovered
device. Or start a new config from `esphome/spooky-overhead-<strip>.yaml`. Then uncomment and
change the substitutions:

```yaml
substitutions:
  name: spooky-overhead
  friendly_name: Spooky Overhead
  mic_pin: "33"
  function_button_pin: GPIO0
  red_pin: GPIO19
  green_pin: GPIO18
  blue_pin: GPIO17
  warm_white_pin: GPIO16
  cold_white_pin: GPIO4
```

Add `wifi: {ssid: …, password: …}` and an `api: encryption: key:` if you like, then **Install**.
The prebuilt files have no API key, so Home Assistant adds them without asking for one.

## 4. Check the colours

Press **Channel test** (a diagnostic button on the device). It lights each output in turn at
60 % for 1.5 s: red, green, blue, then warm and cold white. If red lights green, swap the
substitutions (section 3C).

## 5. In Home Assistant

| Entity | What |
|---|---|
| **light** *Spooky Overhead* | Normal light. Its **colour is the effects' base colour** and its brightness their brightness. Effect list: the ten Halloween effects by name. |
| select *Sound source* | Off · Microphone · Home Assistant · Mic + Home Assistant |
| number *Effect speed* | 1–100 |
| number *Mic sensitivity* | 1–100 (config) |
| number *Sound cue* | written by the Spooky Sound integration. Leave it alone. |
| sensor *Sound level* | 0–100 %, at most once a second |
| event *Sound heard* | `thunder`, `fire`, `loud` |
| binary sensor *Microphone* | is a mic plugged in? (diagnostic) |
| button *Restart*, *Channel test* | |

The **Function button**: a click goes to the next effect (after the last one: no effect); hold
1–5 s to switch the light on or off.

### Cue it from an automation

The firmware has an action, `esphome.<device>_cue`:

```yaml
# Doorbell → a thunderclap overhead
action: esphome.spooky_overhead_1a2b3c_cue
data:
  cue: thunder        # thunder · rumble · fire · crackle · beat · peak · clear
  strength: 0.9       # 0–1 (for crackle: how many, 1–30)
```

### Follow the soundtrack (Spooky Sound integration)

Add the overhead light to the **Spooky Sound** integration (*Settings → Devices & services →
Add integration → Spooky Sound*) and pick *Spooky Overhead*. It maps:

* **Effect** → the light itself. A thunderstorm track switches it to *Lightning Storm* by
  name; when playback stops it goes back to what it was. A light that's off stays off.
* **Sound cue** → the *Sound cue* number, so the **Sendspin** bridge's timed cues reach the
  overhead strip too.

The overhead strip's own sound source, sensitivity, level and event entities are native ESPHome
entities, so the integration doesn't duplicate them. Run one Spooky Sound entry per light: the
XIAO by the speaker and the strip overhead can follow the same media player.

Over Wi-Fi a cue arrives in roughly 10–30 ms, faster than over Zigbee. If the strikes land a hair
late or early, set that entry's lead time lower (try 40 ms).

## How six flames become one ceiling

The effects were written for a chain of separate LEDs: six candles each flicker on their own,
and a crackle flares *one* of them white-hot. A single analog strip is one colour. So the
component renders six virtual pixels and folds them into one colour, per effect:

| Effect | Mix | Why |
|---|---|---|
| Candle Flicker, Possessed Pumpkin | 85 % average + 15 % brightest | a crackle shows as a warm flare (+70 %), not a strobe |
| Witch's Cauldron | 60 / 40 | bubbles pop visibly |
| Lightning Storm | 50 / 50 | a strike on one "cloud" still lights the room |
| Poltergeist | 30 / 70 | violent and jumpy, as intended |
| Ghost Breath, Tell-Tale Heart, Haunted Chase, Blood Moon, Solid | pixel 0 | already move as one |

**Whites.** On an RGBW strip the shared part of R, G and B goes to the white LEDs. On RGBCCT,
bluish whites (a lightning bolt) go to the **cold** LEDs and warm whites (a flame's core) to the
**warm** LEDs. A strike overhead is a real 6500 K flash, not R+G+B pretending.
