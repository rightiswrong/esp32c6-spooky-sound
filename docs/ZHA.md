# Spooky Sound with ZHA

ZHA (Zigbee Home Automation, built into Home Assistant) is the supported way to run this
light. Nothing custom is needed on the ZHA side: there's no quirk and no converter, because
every endpoint is a standard ZCL cluster. This page explains what ZHA does with them. It was
checked against ZHA's own source (`zigpy/zha`), so the details below are how ZHA behaves, not
guesses.

## Pairing

1. Settings → Devices & services → **Zigbee Home Automation** → **Add device**.
2. Power the XIAO. The OLED's bottom line shows `..` while it searches and `ZB` once joined.
3. ZHA interviews it and adds **SpookyLab XIAO-C6-SpookySound**.
4. Add **Spooky Sound** (Settings → Devices & services → Add integration). If this is the only
   SpookyLab light in ZHA without Spooky Sound, it's already selected.

The XIAO joins as a **router**. It's mains-powered, so it relays traffic for battery sensors
nearby.

To move it to another network, hold the button **5 s** until the LEDs turn red, then release.
That does a Zigbee factory reset; then pair again.

## What ZHA creates

| Endpoint | Cluster | ZHA entity | Named (from the firmware) | Spooky Sound |
|---|---|---|---|---|
| 10 | Color light | light | — | used by Spooky Lights |
| 11 | Analog Input / Output | sensor / number | Grove LEDs detected / Manual LED count (0 = auto) | — |
| 12 | Analog Output | number | Effect | follows the soundtrack |
| 13 | Analog Output | number | Effect speed | — |
| 14 | Analog Output | number | Output (0 auto,1 onboard,2 grove,3 both) | — |
| 15 | Analog Output | number | Spotlight pixel (0 = off) | — |
| 16 | Analog Output | number | Sound source (0 off,1 mic,2 remote,3 both) | → **Sound source** select (raw entity hidden) |
| 17 | Analog Output | number | Mic sensitivity | — (use it directly) |
| 18 | Analog Output | number | Sound cue | written by Spooky Sound (raw entity hidden) |
| 19 | Analog Input | sensor | Sound level | — |
| 20 | Analog Input | sensor | Sound event | → **Sound heard** event (raw entity hidden) |

* **Names.** ZHA names these entities from each endpoint's ZCL `description` attribute, which
  it reads once, when the device is interviewed.
* **Unique IDs.** Numbers get `<ieee>-<endpoint>-13`; sensors get
  `<ieee>-<endpoint>-12-analog_input`. Spooky Sound's auto-mapping recognises both, by name or
  by endpoint number.
* **Hidden entities.** Spooky Sound *hides* (never disables) the three raw entities it replaces.
  It still reads and writes them. Removing Spooky Sound shows them again. Entities you hid
  yourself are left alone.

## Reporting, and why you don't wait 30 seconds

When it pairs the light, ZHA binds the Analog Input and Analog Output clusters and configures
reporting on their present values: at most every **30 s**, at least every 15 min, on a change
of 1. On its own, that would make a "Sound heard" event up to 30 s late.

It isn't, because the firmware doesn't rely on scheduled reporting for anything time-sensitive.
On a sound event, a chain change or a level change it sends an explicit *Report Attributes*
command straight away, through the binding ZHA created. The scheduled reports just repeat the
same values, and Spooky Sound ignores repeats. Sound-level reports are self-limited to once per
second, and only on a change of 3 % or more, so the mesh stays quiet.

Cues go the other way. Spooky Sound calls `number.set_value` on *Sound cue*, ZHA writes the
attribute, and the firmware reacts in its Zigbee callback. Typically that's one mesh round trip,
tens of milliseconds. It's the delay that *Light lead time* (Sendspin) compensates for. Cues
carry a sequence number, so when ZHA re-reads the attribute it's never mistaken for a new cue.

## Updating the firmware

* **Same endpoints** (the usual case, e.g. 0.1 → 0.2): flash only the app and the light stays
  paired. Use the release's `-app.bin`:

  ```
  esptool --chip esp32c6 write_flash 0x10000 xiao-spooky-sound-<version>-app.bin
  ```

  An Arduino IDE upload also keeps the pairing (leave "Erase All Flash" disabled). The
  **`-factory.bin` image covers the whole 4 MB flash**, so it erases the Zigbee network and your
  settings. That's fine for a first install, but after it you pair again (ZHA keeps the same
  device, because the IEEE address doesn't change).
* **Endpoints added or changed:** ZHA learned the endpoint list and descriptions at the
  interview. Remove the device in ZHA, pair it again, then open **Spooky Sound → Configure**,
  where the entities are re-mapped automatically. Release notes will say when this is needed.

## Troubleshooting

| Symptom | Fix |
|---|---|
| No *Sound level*, *Sound event* or *Grove LEDs detected* sensors | ZHA only creates entities for generic Analog Input clusters in recent Home Assistant releases. Update HA, then **Reconfigure** the device |
| Entities named "Number", "Number 2", … | The description attributes weren't read during the interview, usually because of a weak link while pairing. Remove the device and pair it again closer to the coordinator (or a router) |
| Spooky Sound found no entities | Open **Configure** and pick them by hand (the table above shows which is which), or check the device is SpookyLab XIAO-C6-SpookySound |
| *Sound heard* never fires | Check that the *Sound source* select includes the microphone, that the OLED shows dB values (not `no mic`), and that *Sound event* changes in ZHA when you clap |
| Flashes arrive late with Sendspin | Raise *Light lead time*. Also check the light's link quality in ZHA's device page; a router nearby helps |
| Identify | ZHA's *Identify* button makes the string blink violet |
