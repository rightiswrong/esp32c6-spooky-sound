// Zigbee2MQTT external converter for XIAO Spooky Sound — BEST EFFORT.
//
// ZHA is the supported way to run this light (see the README). This converter
// is kept for Zigbee2MQTT users but isn't part of the tested path.
//
// ZHA users don't need this — every endpoint uses standard ZCL clusters and ZHA
// creates the entities on its own. Zigbee2MQTT doesn't expose unknown devices'
// analog endpoints, so drop this file into  <z2m data>/external_converters/
// and restart Zigbee2MQTT (2.x loads that folder automatically).
//
// Status: written against zigbee-herdsman-converters' modernExtend API but not
// yet tested on real Zigbee2MQTT hardware — please open an issue if an expose
// misbehaves.

import * as m from "zigbee-herdsman-converters/lib/modernExtend";

const analogOut = (name, endpoint, min, max, description) =>
  m.numeric({
    name,
    cluster: "genAnalogOutput",
    attribute: "presentValue",
    endpointNames: [endpoint],
    valueMin: min,
    valueMax: max,
    valueStep: 1,
    access: "ALL",
    description,
  });

const analogIn = (name, endpoint, description, change) =>
  m.numeric({
    name,
    cluster: "genAnalogInput",
    attribute: "presentValue",
    endpointNames: [endpoint],
    access: "STATE_GET",
    reporting: { min: 0, max: 300, change },
    description,
  });

export default {
  zigbeeModel: ["XIAO-C6-SpookySound"],
  model: "XIAO-C6-SpookySound",
  vendor: "SpookyLab",
  description: "XIAO ESP32-C6 Halloween light that listens: Grove chainable LEDs + Grove microphone",
  extend: [
    m.deviceEndpoints({
      endpoints: {
        light: 10, count: 11, effect: 12, speed: 13, output: 14, spotlight: 15,
        sound: 16, sensitivity: 17, cue: 18, level: 19, event: 20,
      },
    }),
    m.light({ color: true, endpointNames: ["light"] }),
    analogIn("led_count", "count", "Grove LEDs detected on the chain (live)", 1),
    analogOut("manual_led_count", "count", 0, 24, "Force a Grove LED count (0 = auto-detect)"),
    analogOut("effect_index", "effect", 0, 9, "Effect index (see README for names)"),
    analogOut("effect_speed", "speed", 1, 100, "Effect speed, 50 = normal"),
    analogOut("output_mode", "output", 0, 3, "0 auto, 1 onboard, 2 grove, 3 both"),
    analogOut("spotlight_pixel", "spotlight", 0, 25, "Pixel that holds the light colour (0 = off)"),
    analogOut("sound_source", "sound", 0, 3, "0 off, 1 microphone, 2 remote (Home Assistant), 3 both"),
    analogOut("mic_sensitivity", "sensitivity", 1, 100, "Microphone sensitivity, 50 = normal"),
    analogOut("sound_cue", "cue", 0, 9999999, "Sound cue written by the Spooky Sound integration"),
    analogIn("sound_level", "level", "Loudness 0..100 (-60..0 dBFS)", 3),
    analogIn("sound_event", "event", "Last sound heard: seq*10 + type (1 thunder, 2 fire, 3 scream)", 1),
  ],
};
