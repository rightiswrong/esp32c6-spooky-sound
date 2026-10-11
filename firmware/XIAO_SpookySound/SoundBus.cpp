#include "SoundBus.h"
#include "config.h"
#include <math.h>

const char *const SOUND_SOURCE_NAMES[SRC_COUNT] = {"Off", "Microphone", "Remote", "Mic + Remote"};

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

bool decodeCue(float value, DecodedCue &out) {
  if (!(value >= 0.0f) || value > 9999999.0f) return false;   // also rejects NaN
  uint32_t v = (uint32_t)lroundf(value);
  out.seq = (uint16_t)(v / 10000u);
  out.type = (uint8_t)((v / 1000u) % 10u);
  out.param = (uint16_t)(v % 1000u);
  return true;
}

float encodeCue(uint16_t seq, uint8_t type, uint16_t param) {
  if (seq > 999) seq = seq % 999 + 1;
  if (type > 9) type = 0;
  if (param > 999) param = 999;
  return (float)((uint32_t)seq * 10000u + (uint32_t)type * 1000u + param);
}

SoundBus::SoundBus() {
  _mux = portMUX_INITIALIZER_UNLOCKED;
}

uint32_t SoundBus::rnd() {
  _rng ^= _rng << 13;
  _rng ^= _rng >> 17;
  _rng ^= _rng << 5;
  return _rng;
}

void SoundBus::setSource(uint8_t mode) {
  if (mode >= SRC_COUNT) mode = SRC_BOTH;
  portENTER_CRITICAL(&_mux);
  _source = mode;
  portEXIT_CRITICAL(&_mux);
}

// ------------------------------------------------------------ microphone
void SoundBus::micFrame(const SoundFeatures &f, uint32_t nowMs) {
  portENTER_CRITICAL(&_mux);
  _mic = f;
  _micAt = nowMs;
  if (micOn() && (f.level > 0.15f || f.rumble > 0.2f)) activity(nowMs);
  portEXIT_CRITICAL(&_mux);
}

void SoundBus::micHit(const SoundHit &h, uint32_t nowMs) {
  portENTER_CRITICAL(&_mux);
  if (micOn()) {
    if (!_pending.hit || h.strength > _pending.hitStrength) {
      _pending.hit = true;
      _pending.hitStrength = h.strength;
      _pending.hitLow = h.lowShare;
      _pending.hitHigh = h.highShare;
      _pending.hitPersist = h.persistence;
    }
    activity(nowMs);
  }
  portEXIT_CRITICAL(&_mux);
}

void SoundBus::micCrackle(float s, uint32_t nowMs) {
  portENTER_CRITICAL(&_mux);
  if (micOn()) {
    if (_pending.crackles < 255) ++_pending.crackles;
    if (s > _pending.crackleStrength) _pending.crackleStrength = s;
    activity(nowMs);
  }
  portEXIT_CRITICAL(&_mux);
}

// ---------------------------------------------------------------- remote
bool SoundBus::remoteCue(float value, uint32_t nowMs) {
  DecodedCue c;
  if (!decodeCue(value, c)) return false;
  bool accepted = false;
  portENTER_CRITICAL(&_mux);
  if (c.seq != 0 && c.seq == _lastSeq) {
    accepted = false;                        // a re-read of the same write, not a new cue
  } else if (!remoteOn()) {
    _lastSeq = c.seq;
    accepted = false;
  } else {
    _lastSeq = c.seq;
    accepted = true;
    ++_remoteCount;
    const float p = c.param / 999.0f;
    switch (c.type) {
      case CUE_THUNDER:
        _pending.thunder = true;
        if (p > _pending.thunderStrength) _pending.thunderStrength = p;
        activity(nowMs);
        break;
      case CUE_RUMBLE:
        _rRumble = p;
        _rRumbleAt = nowMs;
        if (p > 0.05f) activity(nowMs);
        break;
      case CUE_FIRE:
        _rFire = p;
        _rFireAt = nowMs;
        if (p > 0.05f) activity(nowMs);
        break;
      case CUE_CRACKLE: {
        // Spread the crackles over ~0.4 s so a burst sounds like a fire, not a click.
        uint16_t n = c.param > 30 ? 30 : c.param;
        for (uint16_t i = 0; i < n && _scheduled < MAX_SCHEDULED; ++i)
          _crackleAt[_scheduled++] = nowMs + (rnd() % 400u);
        if (n) activity(nowMs);
        break;
      }
      case CUE_BEAT:
        _pending.beat = true;
        if (p > _pending.beatStrength) _pending.beatStrength = p;
        activity(nowMs);
        break;
      case CUE_PEAK:
        if (!_pending.hit || p > _pending.hitStrength) {
          _pending.hit = true;
          _pending.hitStrength = p;
          _pending.hitLow = 0.0f;
          _pending.hitHigh = 0.0f;
          _pending.hitPersist = 1.0f;     // a cue is a deliberate bang
        }
        activity(nowMs);
        break;
      case CUE_CLEAR:
        _rRumble = _rFire = 0;
        _scheduled = 0;
        break;
      default:
        accepted = false;
        break;
    }
  }
  portEXIT_CRITICAL(&_mux);
  return accepted;
}

// Remote continuous values hold for SOUND_REMOTE_HOLD_MS, then fade out in 1 s.
float SoundBus::held(float value, uint32_t at, uint32_t nowMs) {
  uint32_t age = nowMs - at;
  if (age <= SOUND_REMOTE_HOLD_MS) return value;
  float fade = 1.0f - (age - SOUND_REMOTE_HOLD_MS) / 1000.0f;
  return fade > 0 ? value * fade : 0.0f;
}

// ------------------------------------------------------------------ render
SoundSnapshot SoundBus::take(uint32_t nowMs) {
  portENTER_CRITICAL(&_mux);
  SoundSnapshot s = _pending;
  _pending = SoundSnapshot();
  s.enabled = _source != SRC_OFF;

  if (micOn() && nowMs - _micAt < 500) {
    s.level = _mic.level;
    s.rumble = _mic.rumble;
    s.crackleRate = _mic.crackleRate;
  }
  if (remoteOn()) {
    s.level = fmaxf(s.level, held(_rFire, _rFireAt, nowMs));
    s.rumble = fmaxf(s.rumble, held(_rRumble, _rRumbleAt, nowMs));
    // release any scheduled remote crackles that are due
    uint8_t keep = 0;
    for (uint8_t i = 0; i < _scheduled; ++i) {
      if ((int32_t)(nowMs - _crackleAt[i]) >= 0) {
        if (s.crackles < 255) ++s.crackles;
        float st = 0.45f + 0.5f * ((rnd() & 0xFF) / 255.0f);
        if (st > s.crackleStrength) s.crackleStrength = st;
      } else {
        _crackleAt[keep++] = _crackleAt[i];
      }
    }
    _scheduled = keep;
  } else {
    _scheduled = 0;
  }
  if (!s.enabled) {
    SoundSnapshot off;
    off.enabled = false;
    portEXIT_CRITICAL(&_mux);
    return off;
  }
  s.level = clamp01(s.level);
  s.rumble = clamp01(s.rumble);
  s.quietFor = _everActive ? (nowMs - _lastActivityMs) / 1000.0f : 1e9f;
  portEXIT_CRITICAL(&_mux);
  return s;
}
