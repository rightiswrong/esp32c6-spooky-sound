// =============================================================================
//  SoundAnalyzer — turns raw microphone samples into the few things a
//  Halloween light cares about: how loud, how much low rumble, and the moments
//  when something cracks, bangs or crackles.
//
//  Signal chain (per sample, fixed-point — the ESP32-C6 has no FPU):
//
//    raw 12-bit ──► DC blocker ──► y ─┬─► 2-pole low-pass  300 Hz ─► low band
//                                     ├─► 2-pole high-pass 300 Hz ─► mid band (mid+high)
//                                     ├─► 2-pole high-pass 2 kHz  ─► high band
//                                     └─────────────────────────────► full band
//
//    The low-pass is a pair of one-pole IIRs (fine far below fs/2). The
//    high-passes are pairs of bilinear-transform first-order sections: unity
//    gain at Nyquist, -3 dB at the corner each. (Energy "above 300 Hz" can't
//    be had as E_all - E_low: the low-pass's phase lag means the two aren't
//    orthogonal, and the difference jumps around wildly on bass notes.)
//
//  Per 8 ms frame (128 samples @ 16 kHz, float maths, 125 times a second):
//
//    * HIT     : an onset in either of two views of the sound —
//                  mid band jumps > k·B and > 2x the last frame (8 ms view)
//                  bass averaged over 24 ms jumps > k·B and > 1.6x
//                where B is the running MEAN of the recent past and k = 4
//                (≈ +6 dB) at sensitivity 50. Bass is judged over 3 frames
//                because 8 ms holds only a cycle or two of a 150 Hz roar,
//                whose energy then jitters wildly from frame to frame.
//                The hit is reported once its PEAK frame is known (8 or 16 ms
//                after the onset frame) with its strength, the low/high share
//                of the peak frame's energy, and its PERSISTENCE: energy in the
//                frame after the peak / peak energy.
//    * BOOM    : a hit with body — persistence >= 1.05 (still building 16–24 ms
//                in) and not hiss-dominated (high share < 0.6). Measured on
//                synthetic tests: fire crackles never exceed 1.00, thunder
//                onsets never drop below 1.15, kick drums and screams pass too.
//                A lone door slam or knock (an impulse) does not. Booms — not
//                the spectrum alone — are what keep a crackling fire from
//                striking Lightning or guttering the candles.
//    * CRACKLE : the high band alone jumps > k_c·B — a short click such as a
//                log fire's crackle or a twig snap.
//    * RUMBLE  : low-band energy smoothed over ~0.1 s, relative to a slow floor
//                (falls in ~1 s, rises in ~8 s): a steady roar becomes the floor,
//                a thunder rumble climbs far above it for seconds. Weighted by
//                how bass-heavy the sound is (low share 0.1 → 0, 0.2+ → full):
//                a scream's harmonics leak below 300 Hz (share ≈ 0.07) and
//                would otherwise read as rumble; thunder, even through a small
//                speaker, averages ≈ 0.28.
//    * LEVEL   : loudness above the quiet-room floor (smoothed).
//
//  Strength of any event: s = clamp(log2(E / B) / 6, 0, 1) — so +9 dB → 0.5,
//  +18 dB → 1.0.
//
//  Two kinds of reference level, on purpose:
//    * B, the running mean (rises in ~0.5 s, falls in ~0.25 s, frozen while a
//      hit or crackle is happening): onsets are judged against it, so chords
//      that "beat" (8 ms energy swinging 5x as close notes drift in and out of
//      phase) don't look like onsets
//    * slow floors (fall fast, rise slowly) for the rumble and the quiet-room
//      level, which must not follow a steady noise upwards
//
//  Events for Home Assistant are classified 0.3 s after a BOOM (clicks are
//  never classified), once we know how it rang on:
//    THUNDER = a strong boom (s >= 0.4) whose energy above the background is
//              still >= 50 % of its peak 0.22–0.30 s later — the MEDIAN of
//              those frames, so a hi-hat passing through can't fake it — and
//              which is NOISE, not a tone (periodicity < 0.6). A kick drum
//              has died away by then; thunder hasn't. A boom at least ~as
//              strong inside the window restarts it, so music's next beat
//              never counts as a "tail", while a rolling rumble's weaker swells
//              can't steal the classification from the crack.
//              Synthetic tests: thunder's first boom sustains >= 0.71, music's
//              booms <= 0.42; voice periodicity ≈ 0.9, noise and music < 0.53.
//              Also: a rumble > 0.45 for > 1 s (distant thunder, no crack).
//    LOUD    = a strong, sustained boom that IS periodic: a scream
//    FIRE    = steady crackling (>= 2 per second for 2 s) with no booms
//  PERIODICITY is the best normalised autocorrelation of the last 32 ms of the
//  mid band at lags of 5–12.5 ms. A voice repeats every pitch period, so some
//  multiple of it lands in that range with r ≈ 0.8+. Noise — even the narrow
//  200–500 Hz noise left of thunder after a small speaker — has decorrelated
//  by 5 ms. It is computed once per classified hit (~50k integer MACs).
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

struct SoundFeatures {
  float dbfs = -100;        // absolute level, dB relative to ADC full scale
  float level = 0;          // 0..1 loudness above the quiet-room floor
  float rumble = 0;         // 0..1 sustained low-frequency energy
  float crackleRate = 0;    // crackles per second (smoothed over ~1.5 s)
  float lowShare = 0;       // fraction of this frame's energy below ~300 Hz
  float highShare = 0;      // fraction of this frame's energy above ~2 kHz
};

struct SoundHit {
  float strength = 0;       // 0..1, see above
  float lowShare = 0;       // peak frame energy below ~300 Hz, 0..1
  float highShare = 0;      // peak frame energy above ~2 kHz, 0..1
  float persistence = 0;    // frame after the peak / peak (clicks ≈ 0, bangs ≥ 0.5)
};

// Classified events, reported to Home Assistant (values are part of the
// Zigbee protocol — keep in sync with custom_components/spooky_sound/logic.py).
enum SoundEventType : uint8_t {
  SND_EVT_NONE = 0,
  SND_EVT_THUNDER = 1,      // a noisy bang that rolls on, or a long low rumble
  SND_EVT_FIRE = 2,         // steady crackling (a fire, or someone's popcorn)
  SND_EVT_LOUD = 3,         // a shout or a scream
  SND_EVT_COUNT
};

// Per-frame internals, for tuning (serial plotter, host traces).
struct SoundDebug {
  float eAll = 0, eLow = 0, eLow3 = 0, eMid = 0, eHigh = 0;   // mean-square energies
  float bLow3 = 0, bMid = 0, bHigh = 0;                       // their running means
  float lastPeriodicity = 0, lastSustain = 0;                 // of the last classified hit
};

class SoundSink {
public:
  virtual ~SoundSink() {}
  virtual void soundFrame(const SoundFeatures &f) = 0;                    // every frame
  virtual void soundHit(const SoundHit &hit) = 0;
  virtual void soundCrackle(float strength) = 0;
  virtual void soundEvent(SoundEventType type, float strength) = 0;
};

class SoundAnalyzer {
public:
  static constexpr int SAMPLE_RATE = 16000;
  static constexpr int FRAME = 128;                       // 8 ms
  static constexpr float FRAME_S = FRAME / (float)SAMPLE_RATE;
  static constexpr int PERIOD_WINDOW = 512;               // 32 ms of mid band
  static constexpr int PERIOD_LAG_MIN = 80;               // 5 ms
  static constexpr int PERIOD_LAG_MAX = 200;              // 12.5 ms

  // Low band, one-pole in Q15:  a = 1 - exp(-2*pi*fc/fs), fc = 300 Hz -> 0.11114
  static constexpr int32_t A_LOW = 3642;
  // High band, bilinear first-order HPF at 2 kHz, Q15:
  //   k = tan(pi*fc/fs) = 0.41421, b0 = 1/(1+k) = 0.70711, a1 = (1-k)/(1+k) = 0.41421
  //   y[n] = b0*(x[n] - x[n-1]) + a1*y[n-1]
  static constexpr int32_t HP_B0 = 23170;
  static constexpr int32_t HP_A1 = 13573;
  // Mid band, the same section at 300 Hz: k = 0.05897, b0 = 0.94431, a1 = 0.88862
  static constexpr int32_t MP_B0 = 30943;
  static constexpr int32_t MP_A1 = 29118;

  void begin(SoundSink *sink);
  void setSensitivity(uint8_t s);            // 1..100, 50 = normal
  uint8_t sensitivity() const { return _sens; }

  // Feed 16 kHz samples (12-bit, 0..4095). Any chunk size.
  void feed(const uint16_t *raw, size_t n);

  const SoundFeatures &features() const { return _f; }
  const SoundDebug &debug() const { return _dbg; }
  uint32_t frames() const { return _frames; }
  // Raw ADC range seen over the last second (diagnostics: a flat line or a
  // rail value usually means the mic isn't plugged in).
  uint16_t rawMin() const { return _rawMinShown; }
  uint16_t rawMax() const { return _rawMaxShown; }

  // A "boom" has body (persistence >= 1.05) and isn't hiss-dominated (high
  // share < 0.6). Fire crackles fail; thunder, kick drums and screams pass.
  // The effects use the same rule to decide what may strike / gutter.
  static constexpr float BOOM_PERSISTENCE = 1.05f;
  static constexpr float BOOM_MAX_HIGH_SHARE = 0.6f;
  static bool isBoom(const SoundHit &h);

  // Exposed for tests
  static float strengthFromRatio(float ratio);
  // Best normalised autocorrelation of x[0..n) over lags [lagMin, lagMax].
  static float periodicity(const int16_t *x, int n, int lagMin, int lagMax);

private:
  SoundSink *_sink = nullptr;
  uint8_t _sens = 50;
  float _kOnset = 4.0f, _kCrackle = 6.0f, _gate = 340.0f, _gateBase = 340.0f;

  // ---- per-sample state (fixed point)
  int32_t _dc = 2048 << 8;
  bool _dcInit = false;
  int32_t _lp1 = 0, _lp2 = 0;                        // low band
  int32_t _hx1 = 0, _hy1 = 0, _hx2 = 0, _hy2 = 0;    // high band (two HPF stages)
  int32_t _mx1 = 0, _my1 = 0, _mx2 = 0, _my2 = 0;    // mid band (two HPF stages)
  int64_t _sAll = 0, _sLow = 0, _sMid = 0, _sHigh = 0;
  int _k = 0;
  int16_t _midRing[PERIOD_WINDOW] = {};              // last 32 ms of mid band (scaled)
  int _midPos = 0;

  // ---- per-frame state
  SoundFeatures _f;
  SoundDebug _dbg;
  uint32_t _frames = 0;
  float _bMid = 0, _bLow3 = 0, _bHigh = 0;           // running means (onset references)
  float _lowSm = 0, _lowFloor = 0, _allSm = 0;       // rumble: smoothed low & full band, floor
  float _prevMid = 0, _prevLow3 = 0, _prevHigh = 0;
  float _lowHist[3] = {0, 0, 0};
  uint8_t _lowIdx = 0;
  float _roomDb = 0;
  float _hitHold = 0, _crHold = 0;
  int _freezeMid = 0, _freezeHigh = 0;               // frames to keep the means still

  // ---- the hit being measured
  enum HitState : uint8_t { HIT_IDLE, HIT_ONSET, HIT_PEAK_NEXT };
  HitState _hit = HIT_IDLE;
  bool _byMid = false, _byBass = false;
  float _vMid = 0, _vLow3 = 0;                       // view energies at the (current) peak frame
  float _b0Mid = 0, _b0Low3 = 0;                     // backgrounds at the onset
  float _hRatio = 0, _hLow = 0, _hHigh = 0;

  // ---- classification of the latest hit (for Home Assistant)
  float _clsT = -1, _clsS = 0, _clsPeakMid = 0, _clsPeakLow3 = 0, _clsB0Mid = 0, _clsB0Low3 = 0;
  static constexpr int TAIL_FRAMES = 12;
  float _clsTailMid[TAIL_FRAMES] = {}, _clsTailLow3[TAIL_FRAMES] = {}, _clsPeriodicity = 0;
  bool _clsByMid = false, _clsByBass = false, _clsPeriodDone = false;
  int _clsN = 0;
  float _sinceThunder = 99, _sinceLoud = 99, _sinceFire = 99, _sinceBang = 99;
  float _rumbleTime = 0, _fireTime = 0;

  // ---- raw diagnostics
  uint16_t _rawMin = 4095, _rawMax = 0, _rawMinShown = 0, _rawMaxShown = 0;
  int _rawCount = 0;

  void frame();
  void emitHit(float persistence);
  void classify(float eMid, float eLow3);
  float midPeriodicity() const;
};
