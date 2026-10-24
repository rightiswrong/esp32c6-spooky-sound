#include "Effects.h"
#include <math.h>

const char *const EFFECT_NAMES[FX_COUNT] = {
  "Solid",
  "Candle Flicker",
  "Ghost Breath",
  "Lightning Storm",
  "Witch's Cauldron",
  "Tell-Tale Heart",
  "Haunted Chase",
  "Poltergeist",
  "Possessed Pumpkin",
  "Blood Moon",
};

static const float TAU = 6.2831853f;

// ----------------------------------------------------------------- helpers
RGBf mix(RGBf a, RGBf b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
RGBf scale(RGBf a, float k) { return {a.r * k, a.g * k, a.b * k}; }

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// A boom has body and isn't hiss — the same rule the analyser uses to classify
// (SoundAnalyzer::isBoom). Fire crackles fail it; thunder, kicks, screams pass.
static bool heardBoom(const SoundSnapshot *s, float minStrength) {
  if (!s || !s->hit || s->hitStrength < minStrength) return false;
  SoundHit h;
  h.persistence = s->hitPersist;
  h.highShare = s->hitHigh;
  return SoundAnalyzer::isBoom(h);
}
static inline float approach(float cur, float target, float rate, float dt) {
  // frame-rate independent exponential approach
  float k = 1.0f - expf(-rate * dt);
  return cur + (target - cur) * k;
}

RGBf hsv2rgb(float h, float s, float v) {
  h = fmodf(h, 1.0f);
  if (h < 0) h += 1.0f;
  float i = floorf(h * 6.0f), f = h * 6.0f - i;
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch ((int)i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

uint32_t Effects::rnd() {           // xorshift32 — tiny and plenty random for ghosts
  _rng ^= _rng << 13;
  _rng ^= _rng >> 17;
  _rng ^= _rng << 5;
  return _rng;
}
float Effects::rndf() { return (rnd() >> 8) * (1.0f / 16777216.0f); }

// ------------------------------------------------------------ life-cycle
void Effects::begin(uint32_t seed) {
  _rng = seed ? seed : 0x9E3779B9u;
  for (int i = 0; i < MAX_PIXELS; ++i) _wake[i] = 0;
  resetState();
}

void Effects::resetState() {
  for (int i = 0; i < MAX_PIXELS; ++i) {
    _a[i] = 0;
    _b[i] = rndf();
    _c[i] = rndf() * 0.2f;
    _col[i] = {0, 0, 0};
    _spark[i] = 0;
    _spitT[i] = 0;
    _spitS[i] = 0;
  }
  _timer = rndr(1.0f, 3.0f);
  _timer2 = 0;
  _pos = 0;
  _dir = 1;
  _phase = 0;
  _flashes = 0;
  _center = 0;
  _hb = 0;
  _hbPeriod = 1.1f;
  _boost = 0;
}

void Effects::relayout(uint8_t n) {
  if (n > MAX_PIXELS) n = MAX_PIXELS;
  // Newly-arrived pixels start "asleep" and are summoned awake by render().
  for (uint8_t i = _n; i < n; ++i) {
    _wake[i] = 0;
    _a[i] = 0;
    _b[i] = rndf();
    _c[i] = 0;
    _spark[i] = 0;
    _spitT[i] = 0;
  }
  if (_pos >= n) _pos = n ? n - 1 : 0;
  if (_center >= n) _center = n ? n - 1 : 0;
  _n = n;
}

// ------------------------------------------------------------------ render
void Effects::render(uint8_t effect, RGBf *out, uint8_t n, uint32_t dtMs, uint8_t speed, RGBf base,
                     const SoundSnapshot *snd) {
  if (n > MAX_PIXELS) n = MAX_PIXELS;
  if (n != _n) relayout(n);
  if (effect >= FX_COUNT) effect = FX_SOLID;
  if (effect != _lastEffect) {
    resetState();
    _lastEffect = effect;
  }

  float realDt = dtMs * 0.001f;
  if (realDt > 0.1f) realDt = 0.1f;                       // after a stall, don't jump
  if (speed < 1) speed = 1;
  if (speed > 100) speed = 100;
  float factor = powf(2.0f, (speed - 50) / 25.0f);        // 1 → ~0.25x, 50 → 1x, 100 → 4x
  float dt = realDt * factor;
  _t += dt;
  if (_t > 3600.0f) _t -= 3600.0f;                        // keep float precision healthy
  _realDt = realDt;

  // Sound: "listening" eases in quickly when something is heard and lets go
  // slowly after SOUND_LISTEN_MS of quiet, so effects don't snap back.
  _snd = (snd && snd->enabled) ? snd : nullptr;
  float want = (_snd && _snd->quietFor < SOUND_LISTEN_MS / 1000.0f) ? 1.0f : 0.0f;
  _sndMix = approach(_sndMix, want, want > _sndMix ? 4.0f : 0.4f, realDt);

  switch (effect) {
    case FX_SOLID:             fxSolid(out, n, base); break;
    case FX_CANDLE:            fxCandle(out, n, dt, base); break;
    case FX_GHOST_BREATH:      fxGhost(out, n, base); break;
    case FX_LIGHTNING:         fxLightning(out, n, dt, base); break;
    case FX_CAULDRON:          fxCauldron(out, n, dt); break;
    case FX_HEARTBEAT:         fxHeartbeat(out, n, dt, base); break;
    case FX_HAUNTED_CHASE:     fxChase(out, n, dt, base); break;
    case FX_POLTERGEIST:       fxPoltergeist(out, n, dt, base); break;
    case FX_POSSESSED_PUMPKIN: fxPumpkin(out, n, dt); break;
    case FX_BLOOD_MOON:        fxBloodMoon(out, n); break;
    default:                   fxSolid(out, n, base); break;
  }

  // Awakening: a newly plugged-in module shivers with pale ectoplasm for
  // ~1.5 s, then settles into whatever the show is doing.
  const RGBf ecto = {0.55f, 0.75f, 1.0f};
  for (uint8_t i = 0; i < n; ++i) {
    if (_wake[i] >= 1.0f) continue;
    float w = _wake[i];
    float shiver = 0.5f + 0.5f * sinf((1.0f - w) * 37.0f + i);   // ~4 Hz shiver
    out[i] = mix(out[i], scale(ecto, 0.65f * shiver), 1.0f - w);
    _wake[i] = clamp01(w + realDt / 1.5f);
  }
  _snd = nullptr;   // never hold on to the caller's snapshot
}

// ----------------------------------------------------------------- effects
void Effects::fxSolid(RGBf *o, uint8_t n, RGBf base) {
  for (uint8_t i = 0; i < n; ++i) o[i] = base;
}

// Each pixel is its own little flame: a random walk toward a new target every
// 40–160 ms, with the occasional gust that almost blows it out. When a flame
// dims it also reddens (embers are redder than flame tips).
//
// With sound, the flames follow the fire: a quiet hearth burns low and lazy,
// a roaring one tall and lively, and every crackle flares one flame white-hot.
void Effects::fxCandle(RGBf *o, uint8_t n, float dt, RGBf base) {
  const float m = _sndMix;
  const float L = _snd ? _snd->level : 0.0f;
  const float lo = 0.55f + ((0.20f + 0.35f * L) - 0.55f) * m;
  const float hi = 1.00f + ((0.40f + 0.60f * L) - 1.00f) * m;
  const float pace = 1.0f - 0.5f * L * m;

  // A boom (not a crackle — those last only a few ms): every flame gutters
  // at once, then catches again.
  if (heardBoom(_snd, 0.4f)) {
    for (uint8_t i = 0; i < n; ++i) {
      _b[i] = rndr(0.08f, 0.25f);
      _c[i] = rndr(0.08f, 0.18f);
    }
  }
  // Crackles: flare a flame; now and then a spark spits onto its neighbour.
  if (_snd && _snd->crackles && n) {
    uint8_t k = _snd->crackles > 4 ? 4 : _snd->crackles;
    for (uint8_t j = 0; j < k; ++j) {
      uint8_t i = rnd() % n;
      float s = 0.55f + 0.45f * _snd->crackleStrength;
      if (s > _spark[i]) _spark[i] = s;
      if (n > 1 && rndf() < 0.3f) {
        uint8_t nb = (i == 0) ? 1 : (i == n - 1 ? n - 2 : (rnd() & 1 ? i + 1 : i - 1));
        _spitT[nb] = rndr(0.04f, 0.09f);
        _spitS[nb] = 0.6f * s;
      }
    }
  }

  const RGBf ember = {1.0f, 0.30f, 0.04f};
  const RGBf whiteHot = {1.0f, 0.86f, 0.55f};
  for (uint8_t i = 0; i < n; ++i) {
    _c[i] -= dt;
    if (_c[i] <= 0) {
      _b[i] = (rndf() < 0.06f) ? rndr(0.12f, 0.3f) : rndr(lo, hi);
      _c[i] = rndr(0.04f, 0.16f) * pace;
    }
    _a[i] = approach(_a[i], _b[i], 18.0f, dt);
    float lvl = _a[i];
    RGBf c = scale(base, lvl);
    float emberness = 1.0f - lvl;                    // dimmer → redder
    c.g *= 1.0f - 0.55f * emberness;
    c.b *= 1.0f - 0.85f * emberness;

    if (_spitT[i] > 0) {
      _spitT[i] -= _realDt;
      if (_spitT[i] <= 0 && _spitS[i] > _spark[i]) _spark[i] = _spitS[i];
    }
    if (_spark[i] > 0.003f) {
      float s = _spark[i];
      c = mix(c, mix(ember, whiteHot, s), clamp01(s * 1.4f));
      _spark[i] *= expf(-_realDt * 16.0f);          // ~60 ms flare
    } else {
      _spark[i] = 0;
    }
    o[i] = c;
  }
}

// A slow breath that rolls down the chain, as if something pale were drifting
// past each window in turn. Peaks bleach toward white like a sheet in moonlight.
void Effects::fxGhost(RGBf *o, uint8_t n, RGBf base) {
  const RGBf pale = {0.85f, 0.9f, 1.0f};
  const float louder = 1.0f + 0.5f * (_snd ? _snd->level : 0.0f) * _sndMix;
  for (uint8_t i = 0; i < n; ++i) {
    float s = 0.5f - 0.5f * cosf(TAU * (_t / 4.0f) - i * 0.55f);
    float lvl = (0.03f + 0.97f * s * s) * louder;   // squared = longer dark lull
    if (lvl > 1.0f) lvl = 1.0f;
    o[i] = scale(mix(base, pale, 0.35f * s), lvl);
  }
}

// A brooding dark-blue sky; every few seconds a strike made of 1–4 stutters,
// centred somewhere along the chain and spilling to neighbours.
//
// With sound: thunder strikes NOW, sized by how loud it was; the rumble lights
// the clouds from inside; and while the soundtrack keeps rumbling, the storm
// stops inventing its own strikes.
void Effects::fxLightning(RGBf *o, uint8_t n, float dt, RGBf base) {
  const RGBf sky = mix({0.0f, 0.01f, 0.06f}, scale(base, 0.05f), 0.3f);
  const RGBf cloud = {0.16f, 0.18f, 0.36f};
  const RGBf bolt = {0.85f, 0.88f, 1.0f};

  float strike = -1.0f;
  if (_snd) {
    if (_snd->thunder) strike = _snd->thunderStrength;
    // Only a boom strikes: a crackling soundtrack never calls lightning down.
    else if (heardBoom(_snd, 0.12f)) strike = _snd->hitStrength;
  }
  if (strike >= 0.0f) {
    _boltPeak = 0.55f + 0.45f * strike;
    _boltReach = 1.0f + 3.5f * strike;
    int extra = 1 + (int)lroundf(strike * 3.0f);
    if (_phase == 1 && _flashes > 0) {
      _flashes += extra;                            // the storm rolls on
      if (_flashes > 8) _flashes = 8;
    } else {
      _phase = 1;
      _flashes = extra;
      _center = n ? (rnd() % n) : 0;
    }
    _timer = 0;                                     // flash NOW, not after a stutter gap
  }

  _timer -= dt;
  if (_phase == 0 && _timer <= 0) {
    if (_sndMix > 0.5f) {
      _timer = rndr(1.0f, 2.0f);                    // the soundtrack is in charge
    } else {
      _phase = 1;                                   // begin a strike of our own
      _flashes = 1 + (rnd() % 4);
      _center = n ? (rnd() % n) : 0;
      _boltPeak = rndr(0.6f, 1.0f);
      _boltReach = rndr(1.0f, 4.0f);
      _timer = 0;
    }
  }
  if (_phase == 1 && _timer <= 0) {
    if (_flashes > 0) {
      float peak = _boltPeak * rndr(0.75f, 1.0f);
      float reach = _boltReach * rndr(0.7f, 1.2f);
      for (uint8_t i = 0; i < n; ++i) {
        float d = fabsf((float)i - _center) / reach;
        float k = peak * expf(-d * d);
        if (k > _a[i]) _a[i] = k;
      }
      --_flashes;
      _timer = rndr(0.05f, 0.16f);                  // gap until next stutter
    } else {
      _phase = 0;
      _timer = rndr(3.0f, 9.0f);                    // calm before the next storm
    }
  }

  const float rumble = _snd ? _snd->rumble : 0.0f;
  for (uint8_t i = 0; i < n; ++i) {
    _a[i] *= expf(-dt * 14.0f);                     // fast afterglow decay
    RGBf s = sky;
    if (rumble > 0.01f) {
      float w = 0.5f + 0.5f * sinf(_t * 5.3f + i * 1.9f) * sinf(_t * 2.1f + i * 0.7f);
      s = mix(sky, cloud, rumble * (0.3f + 0.4f * w));
    }
    o[i] = mix(s, bolt, clamp01(_a[i]));
  }
}

// Slowly churning emerald ⇄ violet brew; bubbles rise (brighten) and pop.
// Crackles and bangs pop bubbles; a noisy room brings it to a rolling boil.
void Effects::fxCauldron(RGBf *o, uint8_t n, float dt) {
  const RGBf brew = {0.0f, 0.75f, 0.12f};
  const RGBf hex = {0.45f, 0.0f, 0.85f};
  const RGBf foam = {0.6f, 1.0f, 0.3f};
  if (_snd && n) {
    int kicks = _snd->crackles + (_snd->hit ? 2 : 0);
    if (kicks > 4) kicks = 4;
    for (int k = 0; k < kicks; ++k) {
      uint8_t i = rnd() % n;
      if (_a[i] == 0) _a[i] = 0.5f;                 // a bubble bursts up from below
    }
  }
  const float boil = 1.0f + 4.0f * (_snd ? _snd->level : 0.0f) * _sndMix;
  for (uint8_t i = 0; i < n; ++i) {
    float swirl = 0.5f + 0.5f * sinf(_t * 0.7f + _b[i] * TAU + i * 0.9f);
    RGBf c = scale(mix(brew, hex, swirl * swirl), 0.35f + 0.25f * swirl);
    _c[i] -= dt;
    if (_c[i] <= 0 && _a[i] < 0.05f && _a[i] >= 0 && rndf() < dt * 0.8f * boil) {
      _a[i] = 0.01f;                                // a bubble starts to rise
      _c[i] = rndr(0.5f, 2.0f) / boil;
    }
    if (_a[i] > 0) {
      _a[i] += dt * 1.6f * boil;                    // swell...
      if (_a[i] >= 1.0f) _a[i] = -1.0f;             // ...and pop
      else c = mix(c, foam, _a[i] * _a[i]);
    }
    if (_a[i] < 0) {
      _a[i] += dt * 6.0f;                           // pop flash fades fast
      if (_a[i] > 0) _a[i] = 0;
      c = mix(c, {1, 1, 1}, -_a[i] * 0.6f);
    }
    o[i] = c;
  }
}

// lub-dub ... lub-dub. The beat ripples outward from the middle of the chain,
// so a long string looks like it is pumping. With sound, every heard beat (a
// beat cue, or a bassy thump) restarts the "lub" and nudges the tempo.
void Effects::fxHeartbeat(RGBf *o, uint8_t n, float dt, RGBf base) {
  const bool listening = _sndMix > 0.5f;
  _hb += listening ? _realDt : dt;
  if (_snd) {
    bool beat = _snd->beat || (heardBoom(_snd, 0.2f) && _snd->hitLow >= 0.3f);
    if (beat && _hb > 0.3f) {                       // ignore the "dub" of the same beat
      if (_hb > 0.35f && _hb < 1.8f) _hbPeriod = 0.7f * _hbPeriod + 0.3f * _hb;
      _hb = 0;
    }
  }
  if (!listening) _hbPeriod += (1.1f - _hbPeriod) * fminf(1.0f, dt * 0.2f);
  if (_hb > 1000.0f) _hb = fmodf(_hb, _hbPeriod);
  const float period = _hbPeriod;
  const float mid = (n - 1) * 0.5f;
  for (uint8_t i = 0; i < n; ++i) {
    float delay = fabsf(i - mid) * 0.035f;
    // Phase in [0, period). (fmodf(x + 100*period, period) looks equivalent but
    // can round to ~period instead of 0 — and a "lub" exactly on the beat
    // would then render dark.)
    float ph = _hb - delay;
    if (ph < 0) ph += ceilf(-ph / period) * period;
    ph = fmodf(ph, period);
    float lub = expf(-ph * 14.0f);
    float dubT = ph - 0.27f * period / 1.1f;
    float dub = dubT > 0 ? 0.7f * expf(-dubT * 12.0f) : 0.0f;
    float lvl = 0.06f + 0.94f * clamp01(lub + dub);
    o[i] = scale(base, lvl);
  }
}

// One glowing eye with a comet tail prowls the chain, sometimes doubling back.
// On a single LED it becomes an eye that watches... and blinks.
// A bang startles it: it whips round and bolts.
void Effects::fxChase(RGBf *o, uint8_t n, float dt, RGBf base) {
  if (_snd && _snd->hit && _snd->hitStrength >= 0.25f) {
    _dir = -_dir;
    _boost = 1.0f + 2.5f * _snd->hitStrength;
    if (_timer > 0.18f) _timer = 0.18f;             // single LED: a startled blink
  }
  _boost *= expf(-_realDt * 1.5f);
  if (n <= 1) {
    _timer -= dt;
    float lvl = 1.0f;
    if (_timer < 0.18f) lvl = clamp01(fabsf(_timer - 0.09f) / 0.09f);  // the blink
    if (_timer <= 0) _timer = rndr(2.0f, 6.0f);
    if (n) o[0] = scale(base, 0.15f + 0.85f * lvl);
    return;
  }
  _pos += _dir * dt * 5.0f * (1.0f + _boost);       // ~5 pixels per second
  if (_pos >= n - 1) { _pos = n - 1; _dir = -1; }
  if (_pos <= 0) { _pos = 0; _dir = 1; }
  if (rndf() < dt * 0.25f) _dir = -_dir;            // it heard something
  for (uint8_t i = 0; i < n; ++i) {
    _a[i] *= expf(-dt * 4.0f);                      // tail fades
    float d = fabsf(i - _pos);
    if (d < 1.0f && 1.0f - d > _a[i]) _a[i] = 1.0f - d;
    o[i] = scale(base, 0.02f + 0.98f * _a[i] * _a[i]);
  }
}

// Mostly dim... then a random pixel is thrown into a lurid colour and decays,
// and every so often the whole string stutters as if the power was grabbed.
// With sound, bangs do the throwing and a scream grabs the power.
void Effects::fxPoltergeist(RGBf *o, uint8_t n, float dt, RGBf base) {
  const bool listening = _sndMix > 0.5f;
  const float rate = listening ? 0.8f : 3.0f;
  if (n && rndf() < dt * rate) {
    uint8_t i = rnd() % n;
    _col[i] = hsv2rgb(rndf(), rndr(0.7f, 1.0f), 1.0f);
    _a[i] = 1.0f;
  }
  if (_snd && _snd->hit && _snd->hitStrength >= 0.3f && n) {
    int throws = 1 + (int)(_snd->hitStrength * 4.0f);
    for (int k = 0; k < throws; ++k) {
      uint8_t i = rnd() % n;
      _col[i] = hsv2rgb(rndf(), rndr(0.7f, 1.0f), 1.0f);
      _a[i] = 1.0f;
    }
    if (_snd->hitStrength >= 0.7f) _timer2 = 0.25f + 0.35f * _snd->hitStrength;
  }
  _timer -= dt;                                     // stutter scheduler
  if (_timer <= 0) {
    if (!listening) _timer2 = 0.45f;                // stutter duration
    _timer = rndr(6.0f, 14.0f);
  }
  float stutter = 1.0f;
  if (_timer2 > 0) {
    _timer2 -= dt;
    stutter = (fmodf(_timer2, 0.09f) < 0.045f) ? 0.0f : 1.4f;
  }
  for (uint8_t i = 0; i < n; ++i) {
    _a[i] *= expf(-dt * 2.2f);
    RGBf c = mix(scale(base, 0.08f), _col[i], _a[i]);
    o[i] = scale(c, stutter);
  }
}

// A happy jack-o'-lantern candle... until something else moves in. Every
// 10–25 s the flame turns sickly green and jitters, then returns as if nothing
// happened. Ignores the HA colour on purpose: pumpkins are orange.
void Effects::fxPumpkin(RGBf *o, uint8_t n, float dt) {
  const RGBf orange = {1.0f, 0.38f, 0.02f};
  const RGBf sick = {0.35f, 1.0f, 0.0f};
  fxCandle(o, n, dt, orange);
  _timer -= dt;
  if (_phase == 0 && _timer <= 0) { _phase = 1; _timer2 = rndr(1.2f, 2.2f); }
  if (_phase == 1) {
    _timer2 -= dt;
    float jitter = rndr(0.4f, 1.3f);
    for (uint8_t i = 0; i < n; ++i) o[i] = mix(o[i], scale(sick, jitter), 0.85f);
    if (_timer2 <= 0) { _phase = 0; _timer = rndr(10.0f, 25.0f); }
  }
}

// A slow, heavy tide of deep crimson, rust and near-black.
void Effects::fxBloodMoon(RGBf *o, uint8_t n) {
  const RGBf blood = {0.7f, 0.0f, 0.0f};
  const RGBf rust = {1.0f, 0.22f, 0.0f};
  const RGBf night = {0.08f, 0.0f, 0.01f};
  for (uint8_t i = 0; i < n; ++i) {
    float w = 0.5f + 0.5f * sinf(_t * 0.35f - i * 0.45f);
    float w2 = 0.5f + 0.5f * sinf(_t * 0.13f + i * 0.21f + 1.7f);
    RGBf c = w < 0.5f ? mix(night, blood, w * 2.0f) : mix(blood, rust, (w - 0.5f) * 2.0f * w2);
    o[i] = c;
  }
}
