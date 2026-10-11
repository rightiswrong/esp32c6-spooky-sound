#include "SoundAnalyzer.h"
#include <math.h>
#include <string.h>

// Full-scale sine in the Q4 domain: amplitude 2048 counts * 16 = 32768,
// mean square = 32768^2 / 2.
static constexpr float FULL_SCALE_MS = 536870912.0f;
static constexpr float GATE_DBFS = -62.0f;      // below this nothing is an "event"
static constexpr int WARMUP_FRAMES = 60;        // ~0.5 s for the references to settle

static inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

float SoundAnalyzer::strengthFromRatio(float ratio) {
  if (ratio <= 1.0f) return 0.0f;
  return clamp01(log2f(ratio) / 6.0f);
}

void SoundAnalyzer::begin(SoundSink *sink) {
  _sink = sink;
  _gateBase = FULL_SCALE_MS * powf(10.0f, GATE_DBFS / 10.0f);
  setSensitivity(_sens);
}

void SoundAnalyzer::setSensitivity(uint8_t s) {
  if (s < 1) s = 1;
  if (s > 100) s = 100;
  _sens = s;
  // Thresholds scale by f: 50 → ×1, 100 → ×0.11 (much keener), 1 → ×8.4 (deaf-ish)
  float f = powf(2.0f, (50.0f - s) / 16.0f);
  _kOnset = fmaxf(1.6f, 4.0f * f);
  _kCrackle = fmaxf(2.0f, 6.0f * f);
  _gate = _gateBase * f;
}

// ---------------------------------------------------------------- per sample
void SoundAnalyzer::feed(const uint16_t *raw, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint16_t r = raw[i] & 0x0FFF;
    if (r < _rawMin) _rawMin = r;
    if (r > _rawMax) _rawMax = r;
    if (++_rawCount >= SAMPLE_RATE) {
      _rawMinShown = _rawMin;
      _rawMaxShown = _rawMax;
      _rawMin = 4095;
      _rawMax = 0;
      _rawCount = 0;
    }

    int32_t x = (int32_t)r << 8;                 // Q8
    if (!_dcInit) { _dc = x; _dcInit = true; }
    _dc += (x - _dc) >> 10;                      // DC tracker, ~2.5 Hz corner
    int32_t y = (x - _dc) >> 4;                  // Q4 audio, |y| <= 32768

    // 64-bit products: a full-scale step can make (in - state) reach 2^17,
    // and 2^17 * 30943 would overflow 32 bits.
    _lp1 += (int32_t)(((int64_t)(y - _lp1) * A_LOW) >> 15);    // low band: 2 poles @ 300 Hz
    _lp2 += (int32_t)(((int64_t)(_lp1 - _lp2) * A_LOW) >> 15);

    int32_t d1 = y - _hx1;                                       // high band: 2 bilinear HPFs
    _hx1 = y;
    _hy1 = (int32_t)(((int64_t)HP_B0 * d1 + (int64_t)HP_A1 * _hy1) >> 15);
    int32_t d2 = _hy1 - _hx2;
    _hx2 = _hy1;
    _hy2 = (int32_t)(((int64_t)HP_B0 * d2 + (int64_t)HP_A1 * _hy2) >> 15);

    int32_t m1 = y - _mx1;                                       // mid band: 2 bilinear HPFs
    _mx1 = y;
    _my1 = (int32_t)(((int64_t)MP_B0 * m1 + (int64_t)MP_A1 * _my1) >> 15);
    int32_t m2 = _my1 - _mx2;
    _mx2 = _my1;
    _my2 = (int32_t)(((int64_t)MP_B0 * m2 + (int64_t)MP_A1 * _my2) >> 15);

    int32_t ms = _my2 >> 2;                                      // keep 32 ms for periodicity
    _midRing[_midPos] = (int16_t)(ms > 32767 ? 32767 : (ms < -32768 ? -32768 : ms));
    _midPos = (_midPos + 1) % PERIOD_WINDOW;

    _sAll += (int64_t)y * y;
    _sLow += (int64_t)_lp2 * _lp2;
    _sMid += (int64_t)_my2 * _my2;
    _sHigh += (int64_t)_hy2 * _hy2;
    if (++_k == FRAME) frame();
  }
}

// ---------------------------------------------------------------- periodicity
float SoundAnalyzer::periodicity(const int16_t *x, int n, int lagMin, int lagMax) {
  if (lagMax >= n) lagMax = n - 1;
  if (lagMin < 1 || lagMin > lagMax) return 0.0f;
  int64_t total = 0;
  for (int i = 0; i < n; ++i) total += (int32_t)x[i] * x[i];
  if (total <= 0) return 0.0f;
  // head = Σ x[0..L) ², tail = Σ x[n-L..n) ² — so the two overlapping windows'
  // energies are total - tail and total - head.
  int64_t head = 0, tail = 0;
  for (int i = 0; i < lagMin; ++i) {
    head += (int32_t)x[i] * x[i];
    tail += (int32_t)x[n - 1 - i] * x[n - 1 - i];
  }
  float best = 0.0f;
  for (int lag = lagMin; lag <= lagMax; ++lag) {
    int64_t c = 0;
    for (int i = 0; i + lag < n; ++i) c += (int32_t)x[i] * x[i + lag];
    int64_t e1 = total - tail, e2 = total - head;
    if (c > 0 && e1 > 0 && e2 > 0) {
      float r = (float)c / sqrtf((float)e1 * (float)e2);
      if (r > best) best = r;
    }
    head += (int32_t)x[lag] * x[lag];
    tail += (int32_t)x[n - 1 - lag] * x[n - 1 - lag];
  }
  return best;
}

float SoundAnalyzer::midPeriodicity() const {
  static int16_t lin[PERIOD_WINDOW];             // one analyser per device: static is fine
  for (int i = 0; i < PERIOD_WINDOW; ++i) lin[i] = _midRing[(_midPos + i) % PERIOD_WINDOW];
  return periodicity(lin, PERIOD_WINDOW, PERIOD_LAG_MIN, PERIOD_LAG_MAX);
}

// ---------------------------------------------------------------- references
// Running mean for onset decisions: rises in ~0.5 s, falls in ~0.25 s.
static inline void trackMean(float &mean, float e) {
  mean += (e - mean) * (e > mean ? 0.016f : 0.031f);
  if (mean < 1.0f) mean = 1.0f;
}

// ---------------------------------------------------------------- hits
bool SoundAnalyzer::isBoom(const SoundHit &h) {
  return h.persistence >= BOOM_PERSISTENCE && h.highShare < BOOM_MAX_HIGH_SHARE;
}

void SoundAnalyzer::emitHit(float persistence) {
  _hit = HIT_IDLE;
  float s = strengthFromRatio(_hRatio);
  SoundHit hit;
  hit.strength = s;
  hit.lowShare = _hLow;
  hit.highShare = _hHigh;
  hit.persistence = persistence;
  if (_sink) _sink->soundHit(hit);
  // Only booms are classified: a click is never thunder or a scream. A boom
  // at least ~as strong as the one being judged restarts the 0.3 s window
  // (its own tail is what matters now); the weaker swells of a rolling
  // rumble don't, or they would steal the classification from the crack.
  const bool boom = isBoom(hit);
  if (boom && s >= 0.25f) _sinceBang = 0;
  if (boom && (_clsT < 0 || s >= 0.9f * _clsS)) {
    _clsT = 0;
    _clsS = s;
    _clsByMid = _byMid;
    _clsByBass = _byBass;
    _clsPeakMid = _vMid;
    _clsPeakLow3 = _vLow3;
    _clsB0Mid = _b0Mid;
    _clsB0Low3 = _b0Low3;
    _clsN = 0;
    _clsPeriodDone = false;
    _clsPeriodicity = 0;
  }
}

// Called every frame while the latest hit is being classified.
void SoundAnalyzer::classify(float eMid, float eLow3) {
  _clsT += FRAME_S;
  if (_clsT >= 0.22f && _clsN < TAIL_FRAMES) {
    _clsTailMid[_clsN] = eMid;
    _clsTailLow3[_clsN] = eLow3;
    ++_clsN;
  }
  if (!_clsPeriodDone && _clsT >= 0.25f) {
    _clsPeriodicity = midPeriodicity();
    _clsPeriodDone = true;
  }
  if (_clsT < 0.30f) return;

  // How much of the hit, above the background it rose from, is still ringing?
  // (Median of the tail frames: a hi-hat passing through the window must not
  // count as the kick drum "ringing on".)
  auto median = [](float *v, int n) {
    for (int i = 1; i < n; ++i)
      for (int j = i; j > 0 && v[j - 1] > v[j]; --j) { float t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    return n ? v[n / 2] : 0.0f;
  };
  float sustain = 0.0f;
  if (_clsN && _clsByMid && _clsPeakMid > _clsB0Mid)
    sustain = fmaxf(sustain, (median(_clsTailMid, _clsN) - _clsB0Mid) / (_clsPeakMid - _clsB0Mid));
  if (_clsN && _clsByBass && _clsPeakLow3 > _clsB0Low3)
    sustain = fmaxf(sustain, (median(_clsTailLow3, _clsN) - _clsB0Low3) / (_clsPeakLow3 - _clsB0Low3));
  const bool periodic = _clsPeriodicity >= 0.6f;      // voice ≈ 0.9; noise, music < 0.45
  _dbg.lastPeriodicity = _clsPeriodicity;
  _dbg.lastSustain = sustain;

  if (_clsS >= 0.4f && sustain >= 0.5f && !periodic && _sinceThunder > 4.0f) {
    _sinceThunder = 0;
    if (_sink) _sink->soundEvent(SND_EVT_THUNDER, _clsS);
  } else if (_clsS >= 0.6f && sustain >= 0.5f && periodic && _sinceLoud > 1.0f) {
    _sinceLoud = 0;
    if (_sink) _sink->soundEvent(SND_EVT_LOUD, _clsS);
  }
  _clsT = -1;
}

// ---------------------------------------------------------------- per frame
void SoundAnalyzer::frame() {
  const float inv = 1.0f / FRAME;
  const float eAll = (float)_sAll * inv, eLow = (float)_sLow * inv;
  const float eMid = (float)_sMid * inv, eHigh = (float)_sHigh * inv;
  _sAll = _sLow = _sMid = _sHigh = 0;
  _k = 0;
  const float dt = FRAME_S;
  ++_frames;

  _lowHist[_lowIdx] = eLow;
  _lowIdx = (uint8_t)((_lowIdx + 1) % 3);
  const float eLow3 = (_lowHist[0] + _lowHist[1] + _lowHist[2]) / 3.0f;

  // ---- absolute level & quiet-room floor (dB)
  float db = 10.0f * log10f((eAll + 1.0f) / FULL_SCALE_MS);
  if (db < -100.0f) db = -100.0f;
  if (_frames == 1) {
    _roomDb = db;
    _bMid = fmaxf(eMid, 1.0f);
    _bLow3 = fmaxf(eLow3, 1.0f);
    _bHigh = fmaxf(eHigh, 1.0f);
    _lowSm = fmaxf(eLow, 1.0f);
    _allSm = fmaxf(eAll, 1.0f);
    _lowFloor = _lowSm;
    _prevMid = eMid;
    _prevLow3 = eLow3;
    _prevHigh = eHigh;
  }
  if (db < _roomDb) _roomDb += (db - _roomDb) * 0.05f;      // quiet arrives quickly
  else _roomDb += (db - _roomDb) * 0.0002f;                 // noise creeps in over ~40 s
  float target = clamp01((db - _roomDb - 4.0f) / 30.0f);
  _f.level += (target - _f.level) * (target > _f.level ? 0.25f : 0.03f);
  _f.dbfs = db;
  _f.lowShare = eAll > 1.0f ? eLow / eAll : 0.0f;
  _f.highShare = eAll > 1.0f ? eHigh / eAll : 0.0f;

  const bool warm = _frames > WARMUP_FRAMES;
  _hitHold -= dt;
  _crHold -= dt;
  _sinceThunder += dt;
  _sinceLoud += dt;
  _sinceFire += dt;
  _sinceBang += dt;

  _dbg.eAll = eAll;
  _dbg.eLow = eLow;
  _dbg.eLow3 = eLow3;
  _dbg.eMid = eMid;
  _dbg.eHigh = eHigh;
  _dbg.bLow3 = _bLow3;
  _dbg.bMid = _bMid;
  _dbg.bHigh = _bHigh;

  // ---- 1) classification of an earlier hit carries on independently
  if (_clsT >= 0) classify(eMid, eLow3);

  // ---- 2) a hit in progress: find its peak frame, then measure persistence
  const float ratio = fmaxf(eMid / _bMid, eLow3 / _bLow3);
  if (_hit != HIT_IDLE) {
    if (ratio > _hRatio) _hRatio = ratio;
    const bool rose = (_byMid && eMid > _vMid) || (_byBass && eLow3 > _vLow3);
    if (_hit == HIT_ONSET && rose) {
      // The onset straddled a frame boundary: this frame is the real peak.
      _vMid = eMid;
      _vLow3 = eLow3;
      _hLow = eAll > 1.0f ? eLow / eAll : 0.0f;
      _hHigh = eAll > 1.0f ? eHigh / eAll : 0.0f;
      _hit = HIT_PEAK_NEXT;
    } else {
      float persistence = 0.0f;
      if (_byMid && _vMid > 0) persistence = fmaxf(persistence, eMid / _vMid);
      if (_byBass && _vLow3 > 0) persistence = fmaxf(persistence, eLow3 / _vLow3);
      emitHit(persistence);
    }
  }

  // ---- 3) a new onset: mid band (8 ms) or bass (24 ms)
  const bool midOnset = eMid > _gate && eMid > 2.0f * _prevMid && eMid > _kOnset * _bMid;
  const bool bassOnset = eLow3 > _gate && eLow3 > 1.6f * _prevLow3 && eLow3 > _kOnset * _bLow3;
  if (warm && _hit == HIT_IDLE && _hitHold <= 0 && (midOnset || bassOnset)) {
    _hit = HIT_ONSET;
    _byMid = midOnset;
    _byBass = bassOnset;
    _vMid = eMid;
    _vLow3 = eLow3;
    _b0Mid = _bMid;
    _b0Low3 = _bLow3;
    _hRatio = ratio;
    _hLow = eAll > 1.0f ? eLow / eAll : 0.0f;
    _hHigh = eAll > 1.0f ? eHigh / eAll : 0.0f;
    _hitHold = 0.10f;
    _freezeMid = 4;                                   // keep the bang out of the mean
  }

  // ---- 4) high-band crackle
  float crackled = 0;
  if (warm && _crHold <= 0 && eHigh > _gate * 0.5f && eHigh > 2.5f * _prevHigh && eHigh > _kCrackle * _bHigh) {
    float s = strengthFromRatio(eHigh / _bHigh);
    if (_sink) _sink->soundCrackle(s);
    _crHold = 0.016f;
    _freezeHigh = 2;
    crackled = 1;
  }
  const float tau = 1.5f;
  _f.crackleRate = _f.crackleRate * expf(-dt / tau) + crackled / tau;

  // ---- 5) rumble: smoothed low band (~0.1 s) over a slow floor
  _lowSm += (eLow - _lowSm) * 0.077f;
  if (_frames < 250) _lowFloor += (_lowSm - _lowFloor) * 0.05f;         // first 2 s: just learn
  else if (_lowSm < _lowFloor) _lowFloor += (_lowSm - _lowFloor) * 0.008f;   // falls in ~1 s
  else _lowFloor += (_lowSm - _lowFloor) * 0.001f;                      // rises in ~8 s
  if (_lowFloor < 1.0f) _lowFloor = 1.0f;
  // Energy-weighted low share (ratio of smoothed energies, not a smoothed
  // ratio): dominated by whatever is loud right now, not by the quiet before.
  _allSm += (eAll - _allSm) * 0.077f;
  const float bassiness = _allSm > 1.0f ? clamp01((_lowSm / _allSm - 0.10f) / 0.10f) : 0.0f;
  float rTarget = (warm && _lowSm > _gate * 0.5f) ? clamp01(log2f(_lowSm / _lowFloor) / 4.0f) * bassiness : 0.0f;
  _f.rumble += (rTarget - _f.rumble) * 0.05f;

  // ---- 6) slow classifications for Home Assistant
  _rumbleTime = (_f.rumble > 0.45f && _frames > 375) ? _rumbleTime + dt : 0.0f;   // not in the first 3 s
  if (_rumbleTime > 1.0f && _sinceThunder > 8.0f) {
    _sinceThunder = 0;
    if (_sink) _sink->soundEvent(SND_EVT_THUNDER, 0.6f * _f.rumble);
  }
  // Fire: steady crackling, without bangs, at a modest volume.
  const bool fireish = _f.crackleRate >= 2.0f && _sinceBang > 1.5f && _f.level < 0.8f;
  _fireTime = fireish ? _fireTime + dt : 0.0f;
  if (_fireTime > 2.0f && _sinceFire > 30.0f) {
    _sinceFire = 0;
    if (_sink) _sink->soundEvent(SND_EVT_FIRE, clamp01(_f.crackleRate / 8.0f));
  }

  // ---- 7) references (after the comparisons, so an onset is judged against
  //         the past; frozen for a few frames around a transient)
  if (_freezeMid > 0) {
    --_freezeMid;
  } else {
    trackMean(_bMid, eMid);
    trackMean(_bLow3, eLow3);
  }
  if (_freezeHigh > 0) --_freezeHigh;
  else trackMean(_bHigh, eHigh);
  _prevMid = eMid;
  _prevLow3 = eLow3;
  _prevHigh = eHigh;

  if (_sink) _sink->soundFrame(_f);
}
