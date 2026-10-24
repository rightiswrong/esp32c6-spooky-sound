#include "show_core.h"
#include <math.h>
#include <string.h>

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// ------------------------------------------------------------- the mixer
StripMix strip_mix_for(uint8_t effect) {
  switch (effect) {
    case FX_CANDLE:
    case FX_POSSESSED_PUMPKIN: return {0.85f, 0.15f, false};
    case FX_CAULDRON:          return {0.6f, 0.4f, false};
    case FX_LIGHTNING:         return {0.5f, 0.5f, false};
    case FX_POLTERGEIST:       return {0.3f, 0.7f, false};
    default:                   return {0.0f, 0.0f, true};   // solid, ghost, heart, chase, blood moon
  }
}

RGBf strip_mix(uint8_t effect, const RGBf *px, uint8_t n) {
  if (n == 0) return {0, 0, 0};
  const StripMix m = strip_mix_for(effect);
  if (m.single || n == 1) return px[0];
  RGBf mean = {0, 0, 0};
  uint8_t best = 0;
  float bestSum = -1;
  for (uint8_t i = 0; i < n; ++i) {
    mean.r += px[i].r;
    mean.g += px[i].g;
    mean.b += px[i].b;
    float s = px[i].r + px[i].g + px[i].b;
    if (s > bestSum) {
      bestSum = s;
      best = i;
    }
  }
  const float k = 1.0f / n;
  return {m.mean * mean.r * k + m.max * px[best].r, m.mean * mean.g * k + m.max * px[best].g,
          m.mean * mean.b * k + m.max * px[best].b};
}

// ----------------------------------------------------------- the channels
StripChannels strip_channels(RGBf c, StripMode mode) {
  StripChannels out;
  float r = clamp01(c.r), g = clamp01(c.g), b = clamp01(c.b);
  if (mode != STRIP_RGB) {
    const float w = fminf(r, fminf(g, b));
    if (mode == STRIP_RGBW) {
      out.white = w;
    } else {
      // Bluish whites (a lightning bolt) to the cold LEDs, reddish to the warm.
      const float peak = fmaxf(r, fmaxf(g, b));
      const float cold = peak > 0 ? clamp01(0.5f + 2.0f * (b - r) / peak) : 0.5f;
      out.cold_white = w * cold;
      out.warm_white = w * (1.0f - cold);
    }
    r -= w;
    g -= w;
    b -= w;
  }
  const float peak = fmaxf(r, fmaxf(g, b));
  out.color_brightness = peak;
  if (peak > 1e-6f) {
    out.r = r / peak;
    out.g = g / peak;
    out.b = b / peak;
  }
  return out;
}

// ------------------------------------------------------------- the show
void ShowCore::begin(uint32_t seed) {
  fx_.begin(seed);
  analyzer_.begin(this);
  for (auto &p : canvas_) p = {0, 0, 0};
}

RGBf ShowCore::render(uint8_t effect, uint32_t dt_ms, RGBf base, uint32_t now_ms) {
  SoundSnapshot snd = bus_.take(now_ms);
  fx_.render(effect, canvas_, STRIP_PIXELS, dt_ms, speed_, base, &snd);
  return strip_mix(effect, canvas_, STRIP_PIXELS);
}

bool ShowCore::cue_named(const char *name, float strength, uint32_t now_ms) {
  static const struct { const char *name; uint8_t type; } kCues[] = {
      {"thunder", CUE_THUNDER}, {"rumble", CUE_RUMBLE}, {"fire", CUE_FIRE},  {"crackle", CUE_CRACKLE},
      {"beat", CUE_BEAT},       {"peak", CUE_PEAK},     {"clear", CUE_CLEAR},
  };
  for (const auto &c : kCues) {
    if (strcmp(name, c.name) != 0) continue;
    uint16_t param;
    if (c.type == CUE_CRACKLE) param = (uint16_t) (strength < 1 ? 1 : (strength > 30 ? 30 : lroundf(strength)));
    else param = (uint16_t) lroundf(clamp01(strength) * 999.0f);
    // seq 0 = "no sequence": always accepted (API actions are never re-sent).
    return bus_.remoteCue(encodeCue(0, c.type, param), now_ms);
  }
  return false;
}

SoundEventType ShowCore::take_event(float *strength) {
  portENTER_CRITICAL(&evt_mux_);
  SoundEventType t = (SoundEventType) evt_type_;
  if (strength) *strength = evt_strength_;
  evt_type_ = SND_EVT_NONE;
  portEXIT_CRITICAL(&evt_mux_);
  return t;
}

void ShowCore::soundFrame(const SoundFeatures &f) { bus_.micFrame(f, now_()); }
void ShowCore::soundHit(const SoundHit &hit) { bus_.micHit(hit, now_()); }
void ShowCore::soundCrackle(float strength) { bus_.micCrackle(strength, now_()); }
void ShowCore::soundEvent(SoundEventType type, float strength) {
  portENTER_CRITICAL(&evt_mux_);
  evt_type_ = type;
  evt_strength_ = strength;
  portEXIT_CRITICAL(&evt_mux_);
}
