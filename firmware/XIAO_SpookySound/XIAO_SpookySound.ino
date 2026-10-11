// =============================================================================
//  XIAO Spooky Sound — a Zigbee Halloween light that listens
//  Seeed XIAO ESP32-C6 · XIAO Expansion Board · Grove Chainable RGB LED v2.0
//  · Grove Analog Microphone
// =============================================================================
//
//  Arduino IDE settings (Tools menu), Arduino-ESP32 core 3.3.x or newer:
//    Board ............. "XIAO_ESP32C6"
//    USB CDC On Boot ... Enabled            (Serial log over the USB-C port)
//    Partition Scheme .. "Zigbee ZCZR 4MB with spiffs"
//    Zigbee mode ....... "Zigbee ZCZR (coordinator/router)"
//
//  It joins as a Zigbee ROUTER (it's USB powered), strengthening your mesh.
//
//  Zigbee endpoints (all standard ZCL clusters, so ZHA needs no quirk):
//    10..15  exactly as NanoC6 Spooky Lights: light, LED count, effect, speed,
//            output routing, spotlight — so the Spooky Lights integration
//            drives this light unchanged
//    16  Analog Output .. sound source (0 off, 1 mic, 2 remote, 3 both)
//    17  Analog Output .. mic sensitivity (1..100, 50 = normal)
//    18  Analog Output .. sound cue from Home Assistant (see SoundBus.h)
//    19  Analog Input ... sound level 0..100 (-60..0 dBFS)
//    20  Analog Input ... last detected sound event (seq*10 + type)
//
//  Button (expansion board, or the XIAO's BOOT button):
//    click = next effect · double-click = next sound source
//    hold 1–5 s = on/off · hold 5 s+ = Zigbee factory reset (release to confirm)
// =============================================================================

#ifndef ZIGBEE_MODE_ZCZR
#error "Select Tools > Zigbee mode > 'Zigbee ZCZR (coordinator/router)' and Partition Scheme > 'Zigbee ZCZR 4MB with spiffs'"
#endif

#include <Arduino.h>
#include <Preferences.h>
#include "Zigbee.h"
#include "config.h"
#include "P9813Chain.h"
#include "Effects.h"
#include "Renderer.h"
#include "SoundAnalyzer.h"
#include "SoundBus.h"
#include "MicInput.h"
#include "Oled.h"
#include "ZigbeeCompat.h"

// ---------------------------------------------------------------- objects
static ZigbeeColorDimmableLight zbLight(EP_LIGHT);
static ZigbeeAnalog zbCount(EP_COUNT);
static ZigbeeAnalog zbEffect(EP_EFFECT);
static ZigbeeAnalog zbSpeed(EP_SPEED);
static ZigbeeAnalog zbOutput(EP_OUTPUT);
static ZigbeeAnalog zbSpot(EP_SPOTLIGHT);
static ZigbeeAnalog zbSource(EP_SOUND_SOURCE);
static ZigbeeAnalog zbSens(EP_SENSITIVITY);
static ZigbeeAnalog zbCue(EP_CUE);
static ZigbeeAnalog zbLevel(EP_SOUND_LEVEL);
static ZigbeeAnalog zbEvent(EP_SOUND_EVENT);

static P9813Chain chain;
static Renderer renderer;
static Preferences prefs;
static SoundAnalyzer g_analyzer;
static SoundBus g_bus;
static MicInput g_mic;
static Oled g_oled;

// Shared between the Zigbee task (callbacks) and loop(). The C6 is single-core
// but the Zigbee stack runs in its own FreeRTOS task, so guard the struct.
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static ShowState g_state;
static volatile uint32_t g_lastChangeMs = 0;
static volatile bool g_settingsDirty = false;

static uint8_t g_detected = 0;          // debounced module count from loopback
static int g_candidate = -1;
static uint8_t g_candidateHits = 0;
static bool g_countReportPending = true;
static bool g_wasConnected = false;
static bool g_warnedStuck = false;
static uint32_t g_identifyUntil = 0;
static bool g_buttonPinUsable = true;

// Sound events detected by the mic task, waiting to be reported to HA
static portMUX_TYPE g_evtMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t g_evtType = SND_EVT_NONE;
static volatile float g_evtStrength = 0;
static uint16_t g_evtSeq = 0;
static uint32_t g_lastEvtReport = 0;
static uint8_t g_lastLevelReported = 255;
static uint32_t g_lastLevelReport = 0;
static uint8_t g_lastEvtShown = SND_EVT_NONE;
static uint32_t g_lastEvtShownAt = 0;

// OLED "seismograph": one column per 40 ms → 5 s of history
struct ScopeColumn {
  uint8_t height;   // 0..38
  uint8_t flags;    // bit0 hit, bit1 crackle, bit2 thunder cue
};
static ScopeColumn g_scope[Oled::WIDTH];
static uint8_t g_scopeHead = 0;
static float g_scopeLevel = 0;
static uint8_t g_scopeFlags = 0;

// --------------------------------------------------------------- helpers
static uint8_t clampRound(float v, uint8_t lo, uint8_t hi) {
  long x = lroundf(v);
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  return (uint8_t)x;
}

static ShowState snapshot() {
  portENTER_CRITICAL(&g_mux);
  ShowState s = g_state;
  portEXIT_CRITICAL(&g_mux);
  return s;
}

static void touched() {
  g_lastChangeMs = millis();
  g_settingsDirty = true;
}

// The XIAO has no RGB LED; its orange user LED shows the first pixel's brightness.
static void onboardWrite(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t luma = r > g ? (r > b ? r : b) : (g > b ? g : b);
  analogWrite(PIN_USER_LED, USER_LED_ACTIVE_LOW ? 255 - luma : luma);
}

static uint8_t effectiveChainCount(const ShowState &s) {
  if (s.manualCount) return s.manualCount > MAX_CHAIN ? MAX_CHAIN : s.manualCount;
  return g_detected;
}

// True while the analyser is receiving audio (frames keep arriving).
static bool micAlive(uint32_t now) {
  static uint32_t lastFrames = 0, lastChange = 0;
  uint32_t f = g_analyzer.frames();
  if (f != lastFrames) {
    lastFrames = f;
    lastChange = now ? now : 1;
  }
  return lastChange && now - lastChange < 500 && g_analyzer.micPresent();
}

// Log microphone arrivals and departures (and keep HA's level honest).
static void watchMicPresence(bool connected) {
  static int8_t shown = -2;
  int8_t p = g_analyzer.presence();
  if (p == shown || p == -1) return;
  shown = p;
  if (p == 1) {
    Serial.println("[mic] microphone detected — listening");
  } else {
    Serial.println("[mic] no microphone on the A0 port — following Home Assistant / Music Assistant cues only");
    if (connected) {                         // don't leave a stale loudness behind
      zbLevel.setAnalogInput(0);
      zbLevel.reportAnalogInput();
    }
    g_lastLevelReported = 0;
  }
}

static uint8_t levelPercent(float dbfs) {
  float p = (dbfs + 60.0f) * (100.0f / 60.0f);
  return clampRound(p, 0, 100);
}

// --------------------------------------------------------- the microphone
// Runs in the mic task: forward to the bus (for the effects) and remember
// classified events for loop() to report to Home Assistant.
class MicSink : public SoundSink {
public:
  void soundFrame(const SoundFeatures &f) override { g_bus.micFrame(f, millis()); }
  void soundHit(const SoundHit &hit) override { g_bus.micHit(hit, millis()); }
  void soundCrackle(float s) override { g_bus.micCrackle(s, millis()); }
  void soundEvent(SoundEventType type, float s) override {
    portENTER_CRITICAL(&g_evtMux);
    g_evtType = type;
    g_evtStrength = s;
    portEXIT_CRITICAL(&g_evtMux);
  }
};
static MicSink g_sink;

// ------------------------------------------------------------ persistence
static void loadSettings() {
  prefs.begin("spooky", true);
  ShowState s;
  s.on = prefs.getBool("on", s.on);
  s.level = prefs.getUChar("lvl", s.level);
  s.color.r = prefs.getUChar("r", s.color.r);
  s.color.g = prefs.getUChar("g", s.color.g);
  s.color.b = prefs.getUChar("b", s.color.b);
  s.effect = prefs.getUChar("fx", s.effect);
  s.speed = prefs.getUChar("spd", s.speed);
  s.output = prefs.getUChar("out", s.output);
  s.spotlight = prefs.getUChar("spot", s.spotlight);
  s.manualCount = prefs.getUChar("man", s.manualCount);
  s.soundSource = prefs.getUChar("src", s.soundSource);
  s.sensitivity = prefs.getUChar("sens", s.sensitivity);
  prefs.end();
  if (s.effect >= FX_COUNT) s.effect = FX_CANDLE;
  if (s.output >= OUT_COUNT) s.output = OUT_AUTO;
  if (s.speed < 1 || s.speed > 100) s.speed = 50;
  if (s.soundSource >= SRC_COUNT) s.soundSource = SOUND_DEFAULT_SOURCE;
  if (s.sensitivity < 1 || s.sensitivity > 100) s.sensitivity = SOUND_DEFAULT_SENSITIVITY;
  g_state = s;
}

static void saveSettingsIfQuiet() {
  if (!g_settingsDirty || millis() - g_lastChangeMs < SETTINGS_SAVE_DELAY_MS) return;
  g_settingsDirty = false;
  ShowState s = snapshot();
  prefs.begin("spooky", false);
  prefs.putBool("on", s.on);
  prefs.putUChar("lvl", s.level);
  prefs.putUChar("r", s.color.r);
  prefs.putUChar("g", s.color.g);
  prefs.putUChar("b", s.color.b);
  prefs.putUChar("fx", s.effect);
  prefs.putUChar("spd", s.speed);
  prefs.putUChar("out", s.output);
  prefs.putUChar("spot", s.spotlight);
  prefs.putUChar("man", s.manualCount);
  prefs.putUChar("src", s.soundSource);
  prefs.putUChar("sens", s.sensitivity);
  prefs.end();
  Serial.println("[nvs] settings saved");
}

// ----------------------------------------------------- Zigbee callbacks
// These run in the Zigbee task: store and return, rendering happens in loop().
static void onLightRgb(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  portENTER_CRITICAL(&g_mux);
  g_state.on = state;
  g_state.color = {r, g, b};
  g_state.level = level;
  portEXIT_CRITICAL(&g_mux);
  touched();
}

#define ANALOG_SETTER(fn, field, lo, hi)          \
  static void fn(float v) {                       \
    uint8_t x = clampRound(v, lo, hi);            \
    portENTER_CRITICAL(&g_mux);                   \
    g_state.field = x;                            \
    portEXIT_CRITICAL(&g_mux);                    \
    touched();                                    \
  }
ANALOG_SETTER(onManualCount, manualCount, 0, MAX_CHAIN)
ANALOG_SETTER(onEffect, effect, 0, FX_COUNT - 1)
ANALOG_SETTER(onSpeed, speed, 1, 100)
ANALOG_SETTER(onOutput, output, 0, OUT_COUNT - 1)
ANALOG_SETTER(onSpotlight, spotlight, 0, MAX_PIXELS)

static void onSoundSource(float v) {
  uint8_t x = clampRound(v, 0, SRC_COUNT - 1);
  portENTER_CRITICAL(&g_mux);
  g_state.soundSource = x;
  portEXIT_CRITICAL(&g_mux);
  g_bus.setSource(x);
  touched();
}

static void onSensitivity(float v) {
  uint8_t x = clampRound(v, 1, 100);
  portENTER_CRITICAL(&g_mux);
  g_state.sensitivity = x;
  portEXIT_CRITICAL(&g_mux);
  g_analyzer.setSensitivity(x);
  touched();
}

// Cues are events, not settings: never persisted.
static void onCue(float v) { g_bus.remoteCue(v, millis()); }

static void onIdentify(uint16_t seconds) {
  g_identifyUntil = millis() + (uint32_t)seconds * 1000u;
}

// --------------------------------------------------------- Zigbee setup
static void configureAnalogOut(ZigbeeAnalog &ep, const char *desc, float lo, float hi,
                               void (*cb)(float)) {
  ep.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);
  ep.addAnalogOutput();
  ep.setAnalogOutputDescription(desc);
  ep.setAnalogOutputResolution(1);
  ep.setAnalogOutputMinMax(lo, hi);
  ep.onAnalogOutputChange(cb);
}

static void configureAnalogIn(ZigbeeAnalog &ep, const char *desc, float lo, float hi) {
  ep.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);
  ep.addAnalogInput();
  ep.setAnalogInputDescription(desc);
  ep.setAnalogInputResolution(1);
  ep.setAnalogInputMinMax(lo, hi);
}

static void setupZigbee() {
  zbLight.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);
  zbAttachColorCallback(zbLight, onLightRgb);
  zbLight.onIdentify(onIdentify);

  zbCount.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);
  zbCount.addAnalogInput();
  zbCount.setAnalogInputDescription("Grove LEDs detected");
  zbCount.setAnalogInputResolution(1);
  zbCount.setAnalogInputMinMax(0, MAX_CHAIN);
  zbCount.addAnalogOutput();
  zbCount.setAnalogOutputDescription("Manual LED count (0 = auto)");
  zbCount.setAnalogOutputResolution(1);
  zbCount.setAnalogOutputMinMax(0, MAX_CHAIN);
  zbCount.onAnalogOutputChange(onManualCount);

  configureAnalogOut(zbEffect, "Effect", 0, FX_COUNT - 1, onEffect);
  configureAnalogOut(zbSpeed, "Effect speed", 1, 100, onSpeed);
  configureAnalogOut(zbOutput, "Output (0 auto,1 onboard,2 grove,3 both)", 0, OUT_COUNT - 1, onOutput);
  configureAnalogOut(zbSpot, "Spotlight pixel (0 = off)", 0, MAX_PIXELS, onSpotlight);

  configureAnalogOut(zbSource, "Sound source (0 off,1 mic,2 remote,3 both)", 0, SRC_COUNT - 1, onSoundSource);
  configureAnalogOut(zbSens, "Mic sensitivity", 1, 100, onSensitivity);
  configureAnalogOut(zbCue, "Sound cue", 0, 9999999, onCue);
  configureAnalogIn(zbLevel, "Sound level", 0, 100);
  configureAnalogIn(zbEvent, "Sound event", 0, 99999);

  Zigbee.addEndpoint(&zbLight);
  Zigbee.addEndpoint(&zbCount);
  Zigbee.addEndpoint(&zbEffect);
  Zigbee.addEndpoint(&zbSpeed);
  Zigbee.addEndpoint(&zbOutput);
  Zigbee.addEndpoint(&zbSpot);
  Zigbee.addEndpoint(&zbSource);
  Zigbee.addEndpoint(&zbSens);
  Zigbee.addEndpoint(&zbCue);
  Zigbee.addEndpoint(&zbLevel);
  Zigbee.addEndpoint(&zbEvent);

  if (!Zigbee.begin(ZIGBEE_ROUTER)) {
    Serial.println("[zb] failed to start — rebooting");
    delay(1000);
    ESP.restart();
  }
  Serial.println("[zb] stack started, waiting to join (put HA into pairing mode)");
}

// Push the restored state to the coordinator so HA shows reality after a reboot.
static void publishAll() {
  ShowState s = snapshot();
  zbLight.setLight(s.on, s.level, s.color.r, s.color.g, s.color.b);
  zbCount.setAnalogInputReporting(0, 300, 1);
  zbLevel.setAnalogInputReporting(1, 300, 3);
  zbEvent.setAnalogInputReporting(0, 3600, 1);
  zbCount.setAnalogOutput(s.manualCount);
  zbEffect.setAnalogOutput(s.effect);
  zbSpeed.setAnalogOutput(s.speed);
  zbOutput.setAnalogOutput(s.output);
  zbSpot.setAnalogOutput(s.spotlight);
  zbSource.setAnalogOutput(s.soundSource);
  zbSens.setAnalogOutput(s.sensitivity);
  zbCue.setAnalogOutput(0);
  for (ZigbeeAnalog *ep : {&zbCount, &zbEffect, &zbSpeed, &zbOutput, &zbSpot, &zbSource, &zbSens})
    ep->reportAnalogOutput();
  g_countReportPending = true;
  g_lastLevelReported = 255;
}

// ------------------------------------------------------------- the button
static void pickSoundSource(uint8_t src) {
  portENTER_CRITICAL(&g_mux);
  g_state.soundSource = src;
  portEXIT_CRITICAL(&g_mux);
  g_bus.setSource(src);
  touched();
  Serial.printf("[sound] source: %s\n", SOUND_SOURCE_NAMES[src]);
  static const RGB8 hint[SRC_COUNT] = {{60, 0, 0}, {0, 160, 40}, {0, 60, 200}, {150, 0, 200}};
  renderer.setOverride(hint[src], millis() + 600);
  if (Zigbee.connected()) {
    zbSource.setAnalogOutput(src);
    zbSource.reportAnalogOutput();
  }
}

static void singleClick() {
  portENTER_CRITICAL(&g_mux);
  g_state.effect = (g_state.effect + 1) % FX_COUNT;
  g_state.on = true;
  uint8_t fx = g_state.effect;
  portEXIT_CRITICAL(&g_mux);
  touched();
  Serial.printf("[fx] %s\n", EFFECT_NAMES[fx]);
  if (Zigbee.connected()) {
    zbEffect.setAnalogOutput(fx);
    zbEffect.reportAnalogOutput();
    zbLight.setLightState(true);
  }
}

static void handleButton() {
  static bool wasDown = false;
  static uint32_t downAt = 0;
  static uint32_t clickAt = 0;
  static bool clickPending = false;
  bool down = (g_buttonPinUsable && digitalRead(PIN_BUTTON) == LOW) || digitalRead(PIN_BOOT_BUTTON) == LOW;
  uint32_t now = millis();

  if (down && !wasDown) downAt = now;
  if (down && now - downAt >= BUTTON_FACTORY_RESET_MS)
    renderer.setOverride({180, 0, 0}, now + 100);          // red = "let go to reset"

  if (!down && wasDown) {
    uint32_t held = now - downAt;
    if (held >= BUTTON_FACTORY_RESET_MS) {
      Serial.println("[zb] factory reset");
      Zigbee.factoryReset();                               // erases network & reboots
    } else if (held >= 1000) {
      clickPending = false;
      portENTER_CRITICAL(&g_mux);
      g_state.on = !g_state.on;
      bool on = g_state.on;
      portEXIT_CRITICAL(&g_mux);
      touched();
      if (Zigbee.connected()) zbLight.setLightState(on);
    } else if (held >= 30) {                                // debounced click
      if (clickPending && now - clickAt <= BUTTON_DOUBLE_CLICK_MS) {
        clickPending = false;
        pickSoundSource((snapshot().soundSource + 1) % SRC_COUNT);
      } else {
        clickPending = true;
        clickAt = now;
      }
    }
  }
  if (clickPending && !down && now - clickAt > BUTTON_DOUBLE_CLICK_MS) {
    clickPending = false;
    singleClick();
  }
  wasDown = down;
}

// -------------------------------------------------- chain count bookkeeping
static void handleProbeResult(int r) {
  if (r == P9813Chain::PROBE_LINE_STUCK_HIGH) {
    if (!g_warnedStuck) {
      Serial.println("[chain] sense line is high during the start frame — check the "
                     "loopback divider (10k series + 18k to GND, docs/WIRING.md)");
      g_warnedStuck = true;
    }
    return;
  }
  if (r < 0) return;
  if (r != g_candidate) {
    g_candidate = r;
    g_candidateHits = 1;
  } else if (g_candidateHits < 255) {
    ++g_candidateHits;
  }
  if (g_candidateHits >= CHAIN_DEBOUNCE_PROBES && r != g_detected) {
    Serial.printf("[chain] %u -> %d Grove LED%s %s\n", g_detected, r, r == 1 ? "" : "s",
                  r > g_detected ? "(welcome, new spirit)" : "(one has departed)");
    g_detected = (uint8_t)r;
    g_countReportPending = true;
  }
}

// ------------------------------------------------------- sound → Home Assistant
static void reportSound(bool connected, uint32_t now) {
  uint8_t type;
  float strength;
  portENTER_CRITICAL(&g_evtMux);
  type = g_evtType;
  strength = g_evtStrength;
  if (type != SND_EVT_NONE && now - g_lastEvtReport >= SOUND_EVENT_REPORT_MS) g_evtType = SND_EVT_NONE;
  else type = SND_EVT_NONE;
  portEXIT_CRITICAL(&g_evtMux);

  if (type != SND_EVT_NONE) {
    g_lastEvtReport = now;
    g_lastEvtShown = type;
    g_lastEvtShownAt = now;
    g_evtSeq = (uint16_t)(g_evtSeq % 9999 + 1);
    static const char *const names[] = {"", "thunder", "fire", "loud"};
    Serial.printf("[sound] %s (%.2f)\n", names[type], strength);
    if (connected) {
      zbEvent.setAnalogInput((float)g_evtSeq * 10.0f + type);
      zbEvent.reportAnalogInput();
    }
  }

#if MIC_ENABLED
  if (connected && micAlive(now) && now - g_lastLevelReport >= SOUND_LEVEL_REPORT_MS) {
    uint8_t pct = levelPercent(g_analyzer.features().dbfs);
    if (g_lastLevelReported == 255 || abs((int)pct - (int)g_lastLevelReported) >= 3) {
      g_lastLevelReport = now;
      g_lastLevelReported = pct;
      zbLevel.setAnalogInput(pct);
      zbLevel.reportAnalogInput();
    }
  }
#endif
}

// ------------------------------------------------------------- the display
static void scopeAccumulate(const SoundSnapshot &snd, uint32_t now) {
  static uint32_t lastColumn = 0;
  float lvl = micAlive(now) ? g_analyzer.features().level : 0.0f;
  if (snd.level > lvl) lvl = snd.level;
  if (lvl > g_scopeLevel) g_scopeLevel = lvl;
  if (snd.hit) g_scopeFlags |= 1;
  if (snd.crackles) g_scopeFlags |= 2;
  if (snd.thunder) g_scopeFlags |= 4;
  if (now - lastColumn >= 40) {
    lastColumn = now;
    g_scope[g_scopeHead] = {(uint8_t)lroundf(g_scopeLevel * 38.0f), g_scopeFlags};
    g_scopeHead = (uint8_t)((g_scopeHead + 1) % Oled::WIDTH);
    g_scopeLevel = 0;
    g_scopeFlags = 0;
  }
}

static void drawDisplay(const ShowState &s, uint8_t chainCount, bool connected, uint32_t now) {
  char line[24];
  g_oled.clear();
  // Line 0: what's showing (inverted when the light is off)
  g_oled.text(0, 0, s.effect < FX_COUNT ? EFFECT_NAMES[s.effect] : "?", !s.on);

  // Lines 1–5: the seismograph, newest column on the right
  const int top = 9, bottom = 47;
  for (int x = 0; x < Oled::WIDTH; ++x) {
    const ScopeColumn &c = g_scope[(g_scopeHead + x) % Oled::WIDTH];
    if (c.flags & 5) g_oled.vline(x, top, bottom);                        // a strike
    else if (c.height) g_oled.vline(x, bottom - c.height + 1, bottom);
    if (c.flags & 2) g_oled.pixel(x, top);                                 // a crackle
  }
  g_oled.hline(0, Oled::WIDTH - 1, bottom + 1);

  // Line 6: sound source, sensitivity, LED count
  static const char *const src[SRC_COUNT] = {"SOUND OFF", "MIC", "HA", "MIC+HA"};
  snprintf(line, sizeof(line), "%-7s s%-3u %2uLED", src[s.soundSource % SRC_COUNT], s.sensitivity, chainCount);
  g_oled.text(0, 6, line);

  // Line 7: Zigbee, loudness, latest event
  const char *evt = "";
  if (g_lastEvtShown != SND_EVT_NONE && now - g_lastEvtShownAt < 4000)
    evt = g_lastEvtShown == SND_EVT_THUNDER ? "THUNDER" : g_lastEvtShown == SND_EVT_FIRE ? "FIRE" : "LOUD";
  if (micAlive(now))
    snprintf(line, sizeof(line), "%s %4ddB %s", connected ? "ZB" : "..", (int)lroundf(g_analyzer.features().dbfs), evt);
  else
    snprintf(line, sizeof(line), "%s %s", connected ? "ZB" : "..",
             g_analyzer.presence() == 0 ? "no mic: HA cues" : "mic starting");
  g_oled.text(0, 7, line);
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\n== XIAO Spooky Sound v" FW_VERSION " ==");

  // Antenna: enable the RF switch and pick the on-board ceramic (or u.FL) antenna.
  pinMode(PIN_RF_SWITCH_EN, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_EN, LOW);
  pinMode(PIN_RF_ANT_SELECT, OUTPUT);
  digitalWrite(PIN_RF_ANT_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);

  // Expansion-board peripherals we don't use: keep them quiet.
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  // The user button shares a wire with the mic port's second pin. If it reads
  // "pressed" all through boot, something is holding it low: ignore it.
  delay(5);
  uint32_t lowFor = 0;
  for (int i = 0; i < 20 && digitalRead(PIN_BUTTON) == LOW; ++i, lowFor += 10) delay(10);
  if (lowFor >= 200) {
    g_buttonPinUsable = false;
    Serial.println("[button] GPIO1 is held low (mic module?) — using the BOOT button only");
  }

  pinMode(PIN_USER_LED, OUTPUT);
  onboardWrite(0, 0, 0);

  chain.begin(PIN_GROVE_CLK, PIN_GROVE_DATA, P9813_HALF_PERIOD_US, P9813_SENSE_SETTLE_US);
  renderer.begin(&chain, onboardWrite);
  loadSettings();

  g_bus.setSource(g_state.soundSource);
  g_analyzer.begin(&g_sink);
  g_analyzer.setSensitivity(g_state.sensitivity);
#if MIC_ENABLED
  if (!g_mic.begin(PIN_MIC, &g_analyzer)) Serial.println("[mic] not running — remote cues still work");
#endif

#if OLED_ENABLED
  if (g_oled.begin(OLED_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_HZ)) Serial.println("[oled] display found");
#endif

#if CHAIN_DETECT_LOOPBACK
  // Count the chain before Zigbee starts so the first report is correct.
  for (int i = 0; i < CHAIN_DEBOUNCE_PROBES; ++i) handleProbeResult(renderer.frame(snapshot(), 0, 1, true));
  Serial.printf("[chain] boot scan: %u Grove LED(s)\n", g_detected);
#endif

  setupZigbee();
}

// ------------------------------------------------------------------- loop
void loop() {
  static uint32_t lastFrame = 0, lastProbe = 0, lastDraw = 0, lastDiag = 0;
  uint32_t now = millis();

  handleButton();

  // Zigbee connection state → resync
  bool connected = Zigbee.connected();
  if (connected && !g_wasConnected) {
    Serial.println("[zb] joined — publishing state");
    publishAll();
  }
  g_wasConnected = connected;

  // Identify: violet blink while the coordinator asks "which one are you?"
  if ((int32_t)(g_identifyUntil - now) > 0 && ((now / 250) & 1))
    renderer.setOverride({120, 0, 255}, now + FRAME_INTERVAL_MS + 5);

  // Render
  if (now - lastFrame >= FRAME_INTERVAL_MS) {
    uint32_t dt = now - lastFrame;
    lastFrame = now;
    ShowState s = snapshot();
    SoundSnapshot snd = g_bus.take(now);
    bool probe = now - lastProbe >= CHAIN_PROBE_INTERVAL_MS;
    if (probe) lastProbe = now;
    int r = renderer.frame(s, effectiveChainCount(s), dt, probe, &snd);
    if (probe) handleProbeResult(r);
    scopeAccumulate(snd, now);

    // One OLED page per frame keeps I2C time to ~3 ms per frame.
    if (g_oled.present()) {
      if (now - lastDraw >= 160) {
        lastDraw = now;
        drawDisplay(s, effectiveChainCount(s), connected, now);
      }
      g_oled.service();
    }
  }

  // Tell Home Assistant about a changed chain
  if (connected && g_countReportPending) {
    zbCount.setAnalogInput(g_detected);
    zbCount.reportAnalogInput();
    g_countReportPending = false;
  }
  watchMicPresence(connected);
  reportSound(connected, now);

  if (now - lastDiag >= 10000) {
    lastDiag = now;
    if (g_mic.running() && g_analyzer.micPresent())
      Serial.printf("[mic] raw %u..%u  %.1f dBFS  level %.2f  rumble %.2f  crackle %.1f/s  overruns %u\n",
                    g_analyzer.rawMin(), g_analyzer.rawMax(), g_analyzer.features().dbfs,
                    g_analyzer.features().level, g_analyzer.features().rumble,
                    g_analyzer.features().crackleRate, (unsigned)g_mic.overruns());
  }

  saveSettingsIfQuiet();
  delay(1);
}
