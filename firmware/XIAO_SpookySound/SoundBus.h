// =============================================================================
//  SoundBus — where every source of "something just went bump" meets.
//
//  Two producers, one consumer:
//    * the microphone task  (SoundAnalyzer → micFrame / micHit / micCrackle)
//    * the Zigbee task      (Home Assistant / Music Assistant → remoteCue)
//    * loop()               (take() once per rendered frame → effects)
//
//  Producers only record; take() hands the effects one snapshot per frame:
//  the continuous levels plus every one-shot event since the last frame, so a
//  crackle that arrives between two frames is never lost.
//
//  Remote cue wire format (Zigbee endpoint 18, Analog Output present value):
//
//      value = seq * 10000 + type * 1000 + param
//
//      seq   1..999  increments per cue so a repeated identical cue still counts
//                    (0 = no sequence: always accepted)
//      type  0..9    CueType below
//      param 0..999  strength/level in thousandths, or a count for CRACKLE
//
//  Every value is an integer below 2^24, so it survives the trip through a
//  single-precision ZCL float exactly.
// =============================================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "SoundAnalyzer.h"

enum SoundSourceMode : uint8_t {
  SRC_OFF = 0,
  SRC_MIC = 1,
  SRC_REMOTE = 2,
  SRC_BOTH = 3,
  SRC_COUNT
};

// Keep in sync with custom_components/spooky_sound/logic.py (SOUND_SOURCES).
extern const char *const SOUND_SOURCE_NAMES[SRC_COUNT];

enum CueType : uint8_t {
  CUE_NONE = 0,
  CUE_THUNDER = 1,    // param = strength  → a lightning strike, now
  CUE_RUMBLE = 2,     // param = level     → sky glow (held ~4 s)
  CUE_FIRE = 3,       // param = level     → candle flame height (held ~4 s)
  CUE_CRACKLE = 4,    // param = count     → that many crackles over ~0.4 s
  CUE_BEAT = 5,       // param = strength  → a heartbeat / pulse
  CUE_PEAK = 6,       // param = strength  → a generic bang
  CUE_CLEAR = 9,      // forget all remote state
};

struct DecodedCue {
  uint16_t seq = 0;
  uint8_t type = CUE_NONE;
  uint16_t param = 0;
};

// Pure decoder, exposed for tests. False for negative / out-of-range / NaN.
bool decodeCue(float value, DecodedCue &out);
float encodeCue(uint16_t seq, uint8_t type, uint16_t param);

struct SoundSnapshot {
  bool enabled = false;          // sound source is not "off"
  // continuous, 0..1 (max over the enabled sources)
  float level = 0;
  float rumble = 0;
  float crackleRate = 0;         // per second
  // one-shot events since the previous take()
  uint8_t crackles = 0;
  float crackleStrength = 0;
  bool hit = false;
  float hitStrength = 0, hitLow = 0, hitHigh = 0;
  float hitPersist = 0;          // ~0 for a click/crackle, >= 0.5 for a bang
  bool thunder = false;          // an explicit thunder cue (remote)
  float thunderStrength = 0;
  bool beat = false;
  float beatStrength = 0;
  // seconds since anything was heard (huge if never)
  float quietFor = 1e9f;
};

class SoundBus {
public:
  SoundBus();
  void setSource(uint8_t mode);
  uint8_t source() const { return _source; }

  // Microphone side (mic task). Ignored unless the source includes the mic.
  void micFrame(const SoundFeatures &f, uint32_t nowMs);
  void micHit(const SoundHit &hit, uint32_t nowMs);
  void micCrackle(float strength, uint32_t nowMs);

  // Remote side (Zigbee task). Returns true if the cue was new and accepted.
  bool remoteCue(float value, uint32_t nowMs);

  // Render side (loop): snapshot + clear the one-shot events.
  SoundSnapshot take(uint32_t nowMs);

  uint32_t remoteCueCount() const { return _remoteCount; }

private:
  portMUX_TYPE _mux;
  volatile uint8_t _source = SRC_BOTH;

  // mic
  SoundFeatures _mic;
  uint32_t _micAt = 0;
  // remote continuous values and when they were last refreshed
  float _rRumble = 0, _rFire = 0;
  uint32_t _rRumbleAt = 0, _rFireAt = 0;
  uint16_t _lastSeq = 0;
  uint32_t _remoteCount = 0;
  // pending one-shots
  SoundSnapshot _pending;
  // scheduled remote crackles (ms timestamps)
  static constexpr int MAX_SCHEDULED = 24;
  uint32_t _crackleAt[MAX_SCHEDULED];
  uint8_t _scheduled = 0;
  uint32_t _rng = 0x5EED5EEDu;
  // activity
  uint32_t _lastActivityMs = 0;
  bool _everActive = false;

  uint32_t rnd();
  void activity(uint32_t nowMs) { _lastActivityMs = nowMs; _everActive = true; }
  bool micOn() const { return _source == SRC_MIC || _source == SRC_BOTH; }
  bool remoteOn() const { return _source == SRC_REMOTE || _source == SRC_BOTH; }
  static float held(float value, uint32_t at, uint32_t nowMs);
};
