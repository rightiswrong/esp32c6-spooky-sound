// Host tests for XIAO Spooky Sound firmware logic.
// Build & run: tools/hosttest/run.sh            (add --trace DIR to dump CSVs)
#define SIM_IMPLEMENTATION
#include "sim.h"
#include "Zigbee.h"
ZigbeeCore Zigbee;

// Pull the real sketch into this translation unit (its statics become visible).
#include "../../firmware/XIAO_SpookySound/XIAO_SpookySound.ino"

#include "synth.h"
#include "font5x7.h"
#include <random>
#include <cstdlib>
#include <string>

static int g_fail = 0, g_pass = 0;
static const char *g_traceDir = nullptr;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (cond) { ++g_pass; }                                       \
    else { ++g_fail; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }                   \
  } while (0)

// ============================================================ P9813 (shared)
static uint32_t seeedReference(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t prefix = 0xC0;
  if ((b & 0x80) == 0) prefix |= 0x20;
  if ((b & 0x40) == 0) prefix |= 0x10;
  if ((g & 0x80) == 0) prefix |= 0x08;
  if ((g & 0x40) == 0) prefix |= 0x04;
  if ((r & 0x80) == 0) prefix |= 0x02;
  if ((r & 0x40) == 0) prefix |= 0x01;
  return ((uint32_t)prefix << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static void testChain() {
  printf("[chain] P9813 encoding + live counting 0..%d modules (two chip models)\n", MAX_CHAIN);
  std::mt19937 rng(31);
  int bad = 0;
  for (int i = 0; i < 20000; ++i) {
    RGB8 c{(uint8_t)rng(), (uint8_t)rng(), (uint8_t)rng()};
    if (P9813Chain::encode(c) != seeedReference(c.r, c.g, c.b)) ++bad;
  }
  CHECK(bad == 0, "%d encoding mismatches", bad);

  P9813Chain ch;
  ch.begin(PIN_GROVE_CLK, PIN_GROVE_DATA, 3, 6);
  RGB8 px[MAX_CHAIN];
  for (int i = 0; i < MAX_CHAIN; ++i) px[i] = {(uint8_t)(10 * i + 1), (uint8_t)(200 - 3 * i), (uint8_t)(i * 7)};
  for (ChipModel m : {ChipModel::Consume, ChipModel::ShiftRegister})
    for (int lat = 0; lat <= 1; ++lat)
      for (int n = 0; n <= MAX_CHAIN; ++n) {
        sim.chips.clear();
        sim.setChain(n, m, lat);
        int got = ch.probe(px, MAX_CHAIN, MAX_CHAIN + 1);
        int got2 = ch.probe(px, MAX_CHAIN, MAX_CHAIN + 1);
        CHECK(got == n && got2 == n, "model=%d lat=%d n=%d → %d/%d", (int)m, lat, n, got, got2);
      }
  sim.loopback = false;
  sim.setChain(4);
  CHECK(ch.probe(px, MAX_CHAIN, MAX_CHAIN + 1) == 0, "no loopback should read 0");
  sim.loopback = true;
  sim.stuckHigh = true;
  CHECK(ch.probe(px, MAX_CHAIN, MAX_CHAIN + 1) == P9813Chain::PROBE_LINE_STUCK_HIGH, "stuck high not flagged");
  sim.stuckHigh = false;
}

// ================================================================ cue codec
static void testCueCodec() {
  printf("[cue] Zigbee cue encoding survives a float32 round trip, rejects junk\n");
  int bad = 0;
  for (uint16_t seq : {0, 1, 500, 999})
    for (uint8_t type = 0; type <= 9; ++type)
      for (uint16_t param : {0, 1, 500, 998, 999}) {
        float v = encodeCue(seq, type, param);
        volatile float f32 = v;                       // force a real float32 store
        DecodedCue d;
        if (!decodeCue(f32, d) || d.seq != seq || d.type != type || d.param != param) ++bad;
      }
  CHECK(bad == 0, "%d cue round-trip failures", bad);
  DecodedCue d;
  CHECK(!decodeCue(-1.0f, d), "negative accepted");
  CHECK(!decodeCue(NAN, d), "NaN accepted");
  CHECK(!decodeCue(1e8f, d), "out of range accepted");
  CHECK(encodeCue(1, CUE_THUNDER, 1500) == 11999.0f, "param not clamped");
}

// ============================================================ the analyser
struct Recorder : SoundSink {
  const SoundAnalyzer *an = nullptr;
  struct Hit { float t, s, low, high, persist; };
  std::vector<Hit> hits;
  std::vector<float> crackles;
  std::vector<std::pair<float, SoundEventType>> events;
  std::vector<SoundFeatures> frames;
  float t = 0;                    // end of the latest completed frame
  FILE *trace = nullptr;
  void soundFrame(const SoundFeatures &f) override {
    t += SoundAnalyzer::FRAME_S;
    frames.push_back(f);
    if (trace) {
      const SoundDebug &d = an->debug();
      fprintf(trace, "%.3f,%.2f,%.4f,%.4f,%.3f,%.3f,%.3f,,,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n", t, f.dbfs, f.level,
              f.rumble, f.crackleRate, f.lowShare, f.highShare, d.eMid, d.bMid, d.eLow3, d.bLow3, d.eHigh, d.bHigh, d.eAll);
    }
  }
  void soundHit(const SoundHit &h) override {
    hits.push_back({t, h.strength, h.lowShare, h.highShare, h.persistence});
    mark(SoundAnalyzer::isBoom(h) ? 3 : 1, h.strength);   // 3 = boom, 1 = other hit
  }
  void soundCrackle(float s) override { crackles.push_back(t); mark(2, s); }
  void soundEvent(SoundEventType type, float s) override { events.push_back({t, type}); mark(10 + type, s); }
  void mark(int what, float s) {
    if (trace) fprintf(trace, "%.3f,,,,,,,%d,%.3f,,,,,,,\n", t, what, s);
  }
  int count(SoundEventType type) const {
    int n = 0;
    for (auto &e : events) n += e.second == type;
    return n;
  }
  void endLine() {}
};

static Recorder listen(const std::vector<uint16_t> &adc, uint8_t sensitivity = 50, const char *traceName = nullptr) {
  Recorder rec;
  if (g_traceDir && traceName) {
    std::string path = std::string(g_traceDir) + "/" + traceName + ".csv";
    rec.trace = fopen(path.c_str(), "w");
    if (rec.trace)
      fprintf(rec.trace, "t,dbfs,level,rumble,crackle_rate,low_share,high_share,event,strength,"
                         "e_mid,b_mid,e_low3,b_low3,e_high,b_high,e_all\n");
  }
  SoundAnalyzer an;
  rec.an = &an;
  an.begin(&rec);
  an.setSensitivity(sensitivity);
  for (size_t i = 0; i < adc.size(); i += SoundAnalyzer::FRAME) {
    an.feed(&adc[i], std::min<size_t>(SoundAnalyzer::FRAME, adc.size() - i));
    rec.endLine();
  }
  if (rec.trace) fclose(rec.trace);
  return rec;
}

static void testPeriodicity() {
  printf("[ears] periodicity: a voice repeats, noise (even narrow-band noise) doesn't\n");
  Synth syn;
  auto toI16 = [](const std::vector<float> &x) {
    std::vector<int16_t> v(x.size());
    for (size_t i = 0; i < x.size(); ++i) v[i] = (int16_t)std::lround(x[i] * 8000.0f);
    return v;
  };
  const int W = SoundAnalyzer::PERIOD_WINDOW;
  auto voice = syn.scream(0.2f, 1.0f);
  std::vector<float> seg(voice.begin() + 1000, voice.begin() + 1000 + W);
  Synth::highpass(seg, 300, 2);
  float pv = SoundAnalyzer::periodicity(toI16(seg).data(), W, SoundAnalyzer::PERIOD_LAG_MIN, SoundAnalyzer::PERIOD_LAG_MAX);
  auto white = syn.whiteNoise(W);
  float pw = SoundAnalyzer::periodicity(toI16(white).data(), W, SoundAnalyzer::PERIOD_LAG_MIN, SoundAnalyzer::PERIOD_LAG_MAX);
  auto narrow = syn.whiteNoise(W + 4000);
  Synth::lowpass(narrow, 500, 2);
  Synth::highpass(narrow, 200, 2);
  std::vector<float> nseg(narrow.begin() + 4000, narrow.end());
  Synth::normalize(nseg);
  float pn = SoundAnalyzer::periodicity(toI16(nseg).data(), W, SoundAnalyzer::PERIOD_LAG_MIN, SoundAnalyzer::PERIOD_LAG_MAX);
  printf("         voice %.2f   white noise %.2f   200–500 Hz noise %.2f\n", pv, pw, pn);
  CHECK(pv >= 0.8f, "voice periodicity only %.2f", pv);
  CHECK(pw < 0.3f, "white noise periodicity %.2f", pw);
  CHECK(pn < 0.5f, "narrow-band noise periodicity %.2f", pn);
}

static void testAnalyzerQuiet() {
  printf("[ears] a quiet room (ADC noise only) for 20 s: nothing happens\n");
  Synth syn;
  auto r = listen(syn.toAdc(syn.silence(20)));
  CHECK(r.hits.empty(), "%zu hits in silence", r.hits.size());
  CHECK(r.crackles.size() <= 2, "%zu crackles in silence", r.crackles.size());
  CHECK(r.events.empty(), "%zu events in silence", r.events.size());
  CHECK(r.frames.back().level < 0.05f, "level %.2f in silence", r.frames.back().level);

  printf("[ears] rails and a disconnected (flat) input don't produce events\n");
  for (uint16_t v : {0, 4095, 2048}) {
    std::vector<uint16_t> flat(Synth::n(5), v);
    auto rr = listen(flat);
    CHECK(rr.hits.empty() && rr.events.empty(), "flat %u produced %zu hits", v, rr.hits.size());
  }
}

// Callbacks fire inside the frame being analysed, before its soundFrame(), so
// an event's recorded time is the START of the frame containing it (up to
// 8 ms before the true sample time). Latency = end of that frame - onset.
static float firstHitAfter(const Recorder &r, float t0, float maxDelay) {
  for (auto &h : r.hits) {
    float frameEnd = h.t + SoundAnalyzer::FRAME_S;
    if (frameEnd >= t0 && frameEnd <= t0 + maxDelay) return frameEnd - t0;
  }
  return -1;
}

static void testAnalyzerThunder() {
  printf("[ears] thunder in the room and through a small speaker\n");
  for (bool small : {false, true}) {
    Synth syn;
    auto sig = syn.silence(2.0f);
    Synth::add(sig, syn.thunder(5.0f, 0.35f, small), Synth::n(2.0f));
    Synth::add(sig, syn.silence(2.0f), sig.size());
    auto r = listen(syn.toAdc(sig), 50, small ? "thunder_small_speaker" : "thunder_room");
    float lat = firstHitAfter(r, 2.0f, 0.2f);
    const Recorder::Hit *h = nullptr;
    for (auto &x : r.hits) if (x.t + SoundAnalyzer::FRAME_S >= 2.0f) { h = &x; break; }
    printf("         %-13s hit after %.0f ms, strength %.2f, persistence %.2f, high %.2f, thunder events %d, peak rumble %.2f\n",
           small ? "small speaker" : "room", lat * 1000, h ? h->s : -1, h ? h->persist : -1, h ? h->high : -1,
           r.count(SND_EVT_THUNDER), [&] { float m = 0; for (auto &f : r.frames) m = fmaxf(m, f.rumble); return m; }());
    CHECK(lat >= 0 && lat <= 0.025f, "thunder hit not detected within 25 ms (%.3f)", lat);
    CHECK(h && h->s >= 0.5f, "thunder strength too low: %.2f", h ? h->s : -1);
    CHECK(h && SoundAnalyzer::isBoom({h->s, h->low, h->high, h->persist}),
          "thunder looked like a crackle (persistence %.2f, high %.2f)", h ? h->persist : -1, h ? h->high : -1);
    CHECK(r.count(SND_EVT_THUNDER) >= 1 && r.count(SND_EVT_THUNDER) <= 2, "%d THUNDER events for one clap",
          r.count(SND_EVT_THUNDER));
    CHECK(r.count(SND_EVT_LOUD) == 0, "thunder misreported as LOUD");
    if (!small) {
      float m = 0;
      for (auto &f : r.frames) m = fmaxf(m, f.rumble);
      CHECK(m >= 0.3f, "room thunder rumble only %.2f", m);
    }
  }
}

static void testAnalyzerFire() {
  printf("[ears] a crackling fire: crackles found, no thunder, FIRE reported\n");
  Synth syn;
  std::vector<float> truth;
  auto sig = syn.fire(15.0f, 0.6f, 6.0f, &truth);
  auto r = listen(syn.toAdc(sig), 50, "fire");
  // Each true crackle matched by a detection within 12 ms?
  auto near = [](float c, float t) { return c + SoundAnalyzer::FRAME_S >= t && c <= t + 0.012f; };
  int matched = 0, genuine = 0;
  for (float t : truth)
    for (float c : r.crackles)
      if (near(c, t)) { ++matched; break; }
  for (float c : r.crackles)
    for (float t : truth)
      if (near(c, t)) { ++genuine; break; }
  float recall = truth.empty() ? 0 : (float)matched / truth.size();
  float precision = r.crackles.empty() ? 0 : (float)genuine / r.crackles.size();
  printf("         %zu true crackles, %zu detected, recall %.0f%%, precision %.0f%%, rate %.1f/s\n", truth.size(),
         r.crackles.size(), recall * 100, precision * 100, r.frames.back().crackleRate);
  CHECK(recall >= 0.7f, "crackle recall %.2f", recall);
  CHECK(precision >= 0.7f, "crackle precision %.2f", precision);
  CHECK(r.count(SND_EVT_FIRE) == 1, "expected one FIRE event, got %d", r.count(SND_EVT_FIRE));
  CHECK(r.count(SND_EVT_THUNDER) == 0, "fire reported as thunder");
  int strikes = 0;
  float maxPersist = 0;
  for (auto &h : r.hits) {
    strikes += SoundAnalyzer::isBoom({h.s, h.low, h.high, h.persist}) && h.s >= 0.12f;   // Lightning's rule
    maxPersist = fmaxf(maxPersist, h.persist);
  }
  printf("         %zu crackle-hits, max persistence %.2f → %d would strike Lightning\n", r.hits.size(), maxPersist, strikes);
  CHECK(strikes <= 1, "%d crackles would strike Lightning", strikes);
}

static void testAnalyzerMusic() {
  printf("[ears] music at 120 BPM: kicks are hits, but not THUNDER\n");
  Synth syn;
  std::vector<float> kicks;
  auto sig = syn.music(10.0f, 0.6f, 120, &kicks);
  auto r = listen(syn.toAdc(sig), 50, "music");
  int found = 0;
  for (float k : kicks)
    if (firstHitAfter(r, k, 0.03f) >= 0) ++found;
  printf("         %d of %zu kicks heard (%zu hits), thunder events %d, loud events %d\n", found, kicks.size(), r.hits.size(),
         r.count(SND_EVT_THUNDER), r.count(SND_EVT_LOUD));
  CHECK(found >= (int)kicks.size() * 8 / 10, "only %d/%zu kicks detected", found, kicks.size());
  CHECK(r.count(SND_EVT_THUNDER) == 0, "kick drums reported as thunder (%d)", r.count(SND_EVT_THUNDER));
  CHECK(r.count(SND_EVT_FIRE) == 0, "hi-hats reported as fire");
}

static void testAnalyzerScream() {
  printf("[ears] a trick-or-treater's scream is LOUD, not thunder\n");
  Synth syn;
  auto sig = syn.silence(2.0f);
  Synth::add(sig, syn.scream(0.8f, 0.5f), sig.size());
  Synth::add(sig, syn.silence(1.0f), sig.size());
  auto r = listen(syn.toAdc(sig), 50, "scream");
  CHECK(!r.hits.empty(), "scream not heard");
  CHECK(r.count(SND_EVT_LOUD) == 1, "expected LOUD, got %d", r.count(SND_EVT_LOUD));
  CHECK(r.count(SND_EVT_THUNDER) == 0, "scream reported as thunder");
  float rumble = 0;
  for (auto &f : r.frames) rumble = fmaxf(rumble, f.rumble);
  CHECK(rumble < 0.2f, "a scream reads as rumble (%.2f)", rumble);
  // A long, 1.6 s scream must not become "distant thunder" via the rumble path.
  Synth syn2;
  auto longer = syn2.silence(2.0f);
  Synth::add(longer, syn2.scream(1.6f, 0.5f), longer.size());
  Synth::add(longer, syn2.silence(1.0f), longer.size());
  auto r2 = listen(syn2.toAdc(longer));
  CHECK(r2.count(SND_EVT_THUNDER) == 0, "a long scream reported as thunder");
}

static void testAnalyzerSweep() {
  printf("[ears] robustness sweep: loudness (incl. ADC clipping) × speakers × seeds\n");
  int thunderOk = 0, thunderRuns = 0, fireOk = 0, fireRuns = 0, musicOk = 0, musicRuns = 0;
  for (uint32_t seed : {1u, 2u, 3u}) {
    for (float amp : {0.08f, 0.2f, 0.5f, 1.2f})
      for (bool small : {false, true}) {
        Synth syn;
        syn.rng.seed(seed * 7919u + (uint32_t)(amp * 100) + small);
        auto sig = syn.silence(1.5f);
        Synth::add(sig, syn.thunder(4.0f, amp, small), sig.size());
        auto r = listen(syn.toAdc(sig));
        bool struck = false;
        for (auto &h : r.hits) struck |= h.t >= 1.45f && h.t <= 1.6f && SoundAnalyzer::isBoom({h.s, h.low, h.high, h.persist});
        bool ok = struck && r.count(SND_EVT_THUNDER) >= 1 && r.count(SND_EVT_LOUD) == 0;
        if (!ok) printf("         thunder amp %.2f %s seed %u: struck %d thunder %d loud %d\n", amp, small ? "small" : "room", seed,
                        struck, r.count(SND_EVT_THUNDER), r.count(SND_EVT_LOUD));
        thunderOk += ok;
        ++thunderRuns;
      }
    for (float amp : {0.3f, 0.6f, 1.0f})
      for (float rate : {3.0f, 6.0f, 10.0f}) {
        Synth syn;
        syn.rng.seed(seed * 104729u + (uint32_t)(amp * 10) * 31 + (uint32_t)rate);
        auto r = listen(syn.toAdc(syn.fire(12.0f, amp, rate)));
        int strikes = 0;
        for (auto &h : r.hits) strikes += SoundAnalyzer::isBoom({h.s, h.low, h.high, h.persist}) && h.s >= 0.12f;
        bool ok = strikes <= 1 && r.count(SND_EVT_THUNDER) == 0 && r.count(SND_EVT_LOUD) == 0;
        if (!ok) printf("         fire amp %.1f rate %.0f seed %u: strikes %d thunder %d loud %d\n", amp, rate, seed, strikes,
                        r.count(SND_EVT_THUNDER), r.count(SND_EVT_LOUD));
        fireOk += ok;
        ++fireRuns;
      }
    for (float bpm : {90.0f, 128.0f, 150.0f})
      for (float amp : {0.3f, 0.8f}) {
        Synth syn;
        syn.rng.seed(seed * 15485863u + (uint32_t)bpm + (uint32_t)(amp * 10));
        auto r = listen(syn.toAdc(syn.music(10.0f, amp, bpm)));
        bool ok = r.count(SND_EVT_THUNDER) == 0 && r.count(SND_EVT_FIRE) == 0;
        if (!ok) printf("         music %.0f BPM amp %.1f seed %u: thunder %d fire %d loud %d\n", bpm, amp, seed,
                        r.count(SND_EVT_THUNDER), r.count(SND_EVT_FIRE), r.count(SND_EVT_LOUD));
        musicOk += ok;
        ++musicRuns;
      }
  }
  printf("         thunder %d/%d   fire %d/%d   music %d/%d scenarios behave\n", thunderOk, thunderRuns, fireOk, fireRuns,
         musicOk, musicRuns);
  CHECK(thunderOk == thunderRuns, "thunder misbehaved in %d runs", thunderRuns - thunderOk);
  CHECK(fireOk == fireRuns, "fire misbehaved in %d runs", fireRuns - fireOk);
  CHECK(musicOk == musicRuns, "music misbehaved in %d runs", musicRuns - musicOk);
}

static Recorder listenRaw(const std::vector<uint16_t> &adc, SoundAnalyzer &an) {
  Recorder rec;
  rec.an = &an;
  an.begin(&rec);
  for (size_t i = 0; i < adc.size(); i += SoundAnalyzer::FRAME)
    an.feed(&adc[i], std::min<size_t>(SoundAnalyzer::FRAME, adc.size() - i));
  return rec;
}

static void testNoMicrophone() {
  printf("[no mic] an empty A0 port (pulled down): mic declared absent, total silence\n");
  for (int spikes : {0, 20}) {
    Synth syn;
    SoundAnalyzer an;
    auto r = listenRaw(syn.emptyPort(20.0f, spikes), an);
    float level = 0, rumble = 0;
    for (auto &f : r.frames) { level = fmaxf(level, f.level); rumble = fmaxf(rumble, f.rumble); }
    printf("         %2d spikes/s: presence %d, %zu hits, %zu crackles, %zu events, max level %.2f\n", spikes,
           an.presence(), r.hits.size(), r.crackles.size(), r.events.size(), level);
    CHECK(an.presence() == 0, "empty port not detected (presence %d)", an.presence());
    CHECK(r.hits.empty() && r.crackles.empty() && r.events.empty(), "phantom sounds from an empty port");
    CHECK(level == 0.0f && rumble == 0.0f, "empty port has level %.2f rumble %.2f", level, rumble);
  }
  {
    Synth syn;
    SoundAnalyzer an;
    auto adc = syn.emptyPort(0.3f);
    listenRaw(adc, an);
    CHECK(an.presence() == 0, "absence should be known within 0.25 s (presence %d)", an.presence());
  }

  printf("[no mic] plug the mic in mid-show: no bang from the DC step, then it hears thunder\n");
  {
    Synth syn;
    auto tape = syn.emptyPort(3.0f);
    auto sig = syn.silence(2.5f);
    Synth::add(sig, syn.thunder(3.0f, 0.35f, true), sig.size());
    auto live = syn.toAdc(sig);
    tape.insert(tape.end(), live.begin(), live.end());
    SoundAnalyzer an;
    auto r = listenRaw(tape, an);
    int early = 0;
    bool thunderHit = false;
    for (auto &h : r.hits) {
      if (h.t < 5.4f) ++early;
      if (h.t >= 5.4f && h.t <= 5.6f && SoundAnalyzer::isBoom({h.s, h.low, h.high, h.persist})) thunderHit = true;
    }
    printf("         presence %d, hits before the thunder %d, thunder struck %d, THUNDER events %d\n", an.presence(), early,
           thunderHit, r.count(SND_EVT_THUNDER));
    CHECK(an.presence() == 1, "mic not detected after plugging in");
    CHECK(early == 0, "%d phantom hits around the plug-in", early);
    CHECK(thunderHit, "thunder after plug-in not heard");
    CHECK(r.count(SND_EVT_THUNDER) == 1 && r.count(SND_EVT_LOUD) == 0, "wrong events after plug-in");
  }

  printf("[no mic] pull the mic out mid-show: silence, absent within ~1.25 s\n");
  {
    Synth syn;
    auto tape = syn.toAdc(syn.music(4.0f, 0.4f, 120));
    size_t pulled = tape.size();
    auto gone = syn.emptyPort(3.0f);
    tape.insert(tape.end(), gone.begin(), gone.end());
    SoundAnalyzer an;
    Recorder rec;
    rec.an = &an;
    an.begin(&rec);
    float absentAt = -1;
    for (size_t i = 0; i < tape.size(); i += SoundAnalyzer::FRAME) {
      an.feed(&tape[i], std::min<size_t>(SoundAnalyzer::FRAME, tape.size() - i));
      if (absentAt < 0 && an.presence() == 0) absentAt = (i - (float)pulled) / Synth::FS;
    }
    int after = 0;
    for (auto &h : rec.hits) after += h.t > pulled / Synth::FS + 0.05f;
    for (auto &e : rec.events) after += e.first > pulled / Synth::FS + 0.05f;
    printf("         absent %.2f s after pulling, %d sounds afterwards\n", absentAt, after);
    CHECK(absentAt >= 0 && absentAt <= 1.3f, "unplugged mic declared absent after %.2f s", absentAt);
    CHECK(after == 0, "%d sounds after unplugging", after);
  }
}

static void testAnalyzerSensitivity() {
  printf("[ears] sensitivity: a faint knock is heard at 85, ignored at 10\n");
  Synth syn;
  auto sig = syn.silence(2.0f);
  Synth::add(sig, syn.thunder(0.5f, 0.004f, false), sig.size());
  Synth::add(sig, syn.silence(1.0f), sig.size());
  auto adc = syn.toAdc(sig);
  auto keen = listen(adc, 85);
  auto deaf = listen(adc, 10);
  printf("         hits: keen %zu, deaf %zu\n", keen.hits.size(), deaf.hits.size());
  CHECK(!keen.hits.empty(), "sensitive mic missed the knock");
  CHECK(deaf.hits.empty(), "deaf mic still heard it");
  CHECK(SoundAnalyzer::strengthFromRatio(1.0f) == 0.0f && SoundAnalyzer::strengthFromRatio(64.0f) == 1.0f &&
            fabsf(SoundAnalyzer::strengthFromRatio(8.0f) - 0.5f) < 1e-5f,
        "strength mapping changed");
}

// ================================================================ the bus
static void testBus() {
  printf("[bus] source gating, cue de-duplication, held levels, crackle spreading\n");
  SoundBus bus;
  bus.setSource(SRC_REMOTE);
  SoundHit bang;
  bang.strength = 0.9f;
  bang.lowShare = 0.5f;
  bang.highShare = 0.1f;
  bang.persistence = 1.5f;
  bus.micHit(bang, 1000);
  SoundSnapshot s = bus.take(1000);
  CHECK(!s.hit, "mic hit leaked through in REMOTE mode");
  CHECK(bus.remoteCue(encodeCue(7, CUE_THUNDER, 800), 1000), "thunder cue rejected");
  CHECK(!bus.remoteCue(encodeCue(7, CUE_THUNDER, 800), 1001), "duplicate seq accepted");
  s = bus.take(1002);
  CHECK(s.thunder && fabsf(s.thunderStrength - 800 / 999.0f) < 1e-4f, "thunder not delivered");
  s = bus.take(1003);
  CHECK(!s.thunder, "one-shot delivered twice");

  bus.remoteCue(encodeCue(8, CUE_FIRE, 600), 2000);
  CHECK(fabsf(bus.take(2000 + SOUND_REMOTE_HOLD_MS).level - 0.6f) < 0.01f, "fire level not held");
  float fading = bus.take(2000 + SOUND_REMOTE_HOLD_MS + 500).level;
  CHECK(fading > 0.2f && fading < 0.4f, "fire level should be fading, is %.2f", fading);
  CHECK(bus.take(2000 + SOUND_REMOTE_HOLD_MS + 1200).level == 0.0f, "fire level never faded out");

  bus.remoteCue(encodeCue(9, CUE_CRACKLE, 10), 5000);
  int total = 0, frames = 0;
  for (uint32_t t = 5000; t <= 5450; t += 20) {
    uint8_t c = bus.take(t).crackles;
    total += c;
    frames += c > 0;
  }
  CHECK(total == 10, "expected 10 crackles, got %d", total);
  CHECK(frames >= 5, "crackles bunched into %d frames", frames);

  bus.setSource(SRC_MIC);
  CHECK(!bus.remoteCue(encodeCue(10, CUE_THUNDER, 999), 6000), "cue accepted while remote is off");
  bus.micCrackle(0.5f, 6000);
  CHECK(bus.take(6001).crackles == 1, "mic crackle lost in MIC mode");
  bus.setSource(SRC_OFF);
  bus.micHit(bang, 7000);
  s = bus.take(7001);
  CHECK(!s.enabled && !s.hit, "OFF should silence everything");
}

// ============================================================ the effects
static float brightest(const RGBf *o, int n) {
  float m = 0;
  for (int i = 0; i < n; ++i) m = fmaxf(m, o[i].r + o[i].g + o[i].b);
  return m;
}

static void testEffectsFuzz() {
  printf("[effects] %d effects × sizes × speeds × random sound: finite, bounded, alive\n", FX_COUNT);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0, 1);
  Effects fx;
  fx.begin(1234);
  RGBf out[MAX_PIXELS];
  for (int e = 0; e < FX_COUNT; ++e)
    for (int n : {1, 2, 7, MAX_PIXELS})
      for (int sp : {1, 50, 100})
        for (bool withSound : {false, true}) {
          float lo = 1e9f, hi = -1e9f, prev = -1;
          bool finite = true;
          int changes = 0;
          for (int f = 0; f < 1000; ++f) {
            SoundSnapshot s;
            s.enabled = withSound;
            if (withSound) {
              s.level = u(rng);
              s.rumble = u(rng);
              s.crackles = u(rng) < 0.2f ? (uint8_t)(1 + rng() % 5) : 0;
              s.crackleStrength = u(rng);
              s.hit = u(rng) < 0.05f;
              s.hitStrength = u(rng);
              s.hitLow = u(rng);
              s.hitHigh = u(rng);
              s.hitPersist = u(rng);
              s.thunder = u(rng) < 0.02f;
              s.thunderStrength = u(rng);
              s.beat = u(rng) < 0.04f;
              s.quietFor = u(rng) < 0.5f ? 0.0f : 100.0f;
            }
            fx.render(e, out, n, 20, sp, {1.0f, 0.35f, 0.0f}, &s);
            float sum = 0;
            for (int i = 0; i < n; ++i)
              for (float v : {out[i].r, out[i].g, out[i].b}) {
                if (!std::isfinite(v)) finite = false;
                lo = fminf(lo, v);
                hi = fmaxf(hi, v);
                sum += v;
              }
            if (prev >= 0 && fabsf(sum - prev) > 1e-4f) ++changes;
            prev = sum;
          }
          CHECK(finite, "%s n=%d speed=%d sound=%d produced NaN/inf", EFFECT_NAMES[e], n, sp, withSound);
          CHECK(lo >= -1e-4f && hi <= 1.5f, "%s n=%d speed=%d range [%f, %f]", EFFECT_NAMES[e], n, sp, lo, hi);
          if (e != FX_SOLID) CHECK(changes > 10, "%s n=%d looks frozen", EFFECT_NAMES[e], n);
        }
}

// A "strike" = the first flash after at least 0.5 s without one (a strike's
// own 1–4 stutters count once).
struct StrikeCounter {
  float prev = 0, sinceFlash = 99;
  int strikes = 0;
  void frame(float b) {
    bool flash = b > prev + 0.3f;
    if (flash && sinceFlash >= 0.5f) ++strikes;
    sinceFlash = flash ? 0 : sinceFlash + 0.02f;
    prev = b;
  }
};

static void testLightningListens() {
  printf("[effects] Lightning Storm strikes when the soundtrack thunders, not on its own\n");
  Effects fx;
  fx.begin(99);
  RGBf out[12];
  const RGBf base = {0.4f, 0.4f, 1.0f};
  // 60 s of sound-led show: a thunder hit every 4 s, quiet otherwise.
  StrikeCounter sc;
  int cued = 0, lateOrMissing = 0;
  for (int f = 0; f < 3000; ++f) {
    SoundSnapshot s;
    s.enabled = true;
    s.quietFor = (f % 200) * 0.02f;
    bool cue = f % 200 == 0 && f > 0;
    if (cue) {
      s.hit = true;
      s.hitStrength = 0.8f;
      s.hitLow = 0.6f;
      s.hitHigh = 0.1f;
      s.hitPersist = 1.5f;
      ++cued;
    }
    fx.render(FX_LIGHTNING, out, 12, 20, 50, base, &s);
    float b = brightest(out, 12);
    if (cue && b < 1.5f) ++lateOrMissing;          // the very same frame must flash
    sc.frame(b);
  }
  printf("         %d thunder cues → %d strikes\n", cued, sc.strikes);
  CHECK(lateOrMissing == 0, "%d cues didn't flash in the same frame", lateOrMissing);
  CHECK(sc.strikes <= cued + 1, "random strikes while listening (%d strikes for %d cues)", sc.strikes, cued);

  // Deaf: it still storms by itself, every 3–9 s.
  fx.begin(99);
  StrikeCounter deaf;
  for (int f = 0; f < 3000; ++f) {
    fx.render(FX_LIGHTNING, out, 12, 20, 50, base, nullptr);
    deaf.frame(brightest(out, 12));
  }
  printf("         with no sound: %d strikes of its own in 60 s\n", deaf.strikes);
  CHECK(deaf.strikes >= 5, "a deaf storm should still strike on its own (%d)", deaf.strikes);
}

static void testCandleCrackles() {
  printf("[effects] Candle Flicker: crackles flare white-hot, a loud fire burns taller\n");
  Effects fx;
  fx.begin(5);
  RGBf out[8];
  const RGBf base = {1.0f, 0.35f, 0.0f};
  SoundSnapshot quiet;
  quiet.enabled = true;
  quiet.quietFor = 0;
  for (int f = 0; f < 100; ++f) fx.render(FX_CANDLE, out, 8, 20, 50, base, &quiet);
  SoundSnapshot crack = quiet;
  crack.crackles = 1;
  crack.crackleStrength = 0.8f;
  fx.render(FX_CANDLE, out, 8, 20, 50, base, &crack);
  float whitest = 0;
  for (auto &c : out) whitest = fmaxf(whitest, c.g + c.b);   // base has no blue: blue = spark
  CHECK(whitest > 0.9f, "no white-hot flare after a crackle (%.2f)", whitest);

  auto avg = [&](float level) {
    Effects e;
    e.begin(11);
    SoundSnapshot s;
    s.enabled = true;
    s.quietFor = 0;
    s.level = level;
    double sum = 0;
    for (int f = 0; f < 500; ++f) {
      e.render(FX_CANDLE, out, 8, 20, 50, base, &s);
      if (f > 100) for (auto &c : out) sum += c.r;
    }
    return sum / (399 * 8);
  };
  double low = avg(0.05f), high = avg(0.9f);
  printf("         average flame: quiet fire %.2f, roaring fire %.2f\n", low, high);
  CHECK(high > low + 0.15, "loud fire should burn taller (%.2f vs %.2f)", high, low);
}

static void testHeartLocks() {
  printf("[effects] Tell-Tale Heart locks onto a heard heartbeat (0.8 s period)\n");
  Effects fx;
  fx.begin(3);
  RGBf out[5];
  int aligned = 0, beats = 0;
  for (int f = 0; f < 1500; ++f) {
    SoundSnapshot s;
    s.enabled = true;
    s.quietFor = 0;
    s.beat = (f % 40) == 0;                         // every 0.8 s
    s.beatStrength = 0.8f;
    fx.render(FX_HEARTBEAT, out, 5, 20, 50, {1, 0, 0}, &s);
    if (s.beat && f > 400) {
      ++beats;
      if (out[2].r > 0.85f) ++aligned;              // the middle pixel "lubs" on the beat
    }
  }
  CHECK(aligned >= beats - 1, "heart not on the beat (%d of %d)", aligned, beats);
}

static void testPowerLimiter() {
  printf("[power] full-white 24-module chain stays within %d mA budget\n", POWER_BUDGET_MA);
  Renderer r;
  P9813Chain ch;
  ch.begin(PIN_GROVE_CLK, PIN_GROVE_DATA, 3, 6);
  sim.setChain(MAX_CHAIN);
  r.begin(&ch, [](uint8_t, uint8_t, uint8_t) {});
  ShowState s;
  s.on = true; s.level = 254; s.color = {255, 255, 255}; s.effect = FX_SOLID; s.output = OUT_BOTH;
  for (int i = 0; i < 150; ++i) r.frame(s, MAX_CHAIN, 20, false);
  float ma = 0;
  for (int i = 0; i < MAX_CHAIN; ++i) ma += r.lastChainOut(i) * (MA_PER_CHAIN_CHANNEL / 255.0f);
  CHECK(r.lastPowerScale() < 1.0f, "limiter did not engage");
  CHECK(ma <= POWER_BUDGET_MA + 1, "over budget: %.0f mA", ma);
}

// ===================================================================== OLED
static void testOled() {
  printf("[oled] the framebuffer reaches the panel page by page; no panel = no traffic\n");
  panel = Ssd1306Model();
  Oled o;
  CHECK(o.begin(OLED_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_HZ), "display not found");
  CHECK(panel.displayOn && panel.chargePump, "init sequence incomplete");
  o.text(0, 0, "Lightning Storm");
  o.vline(64, 10, 40);
  int sends = 0;
  while (!o.service()) ++sends;
  CHECK(sends == Oled::PAGES - 1, "full frame took %d services", sends + 1);
  CHECK(memcmp(panel.ram, o.buffer(), sizeof(panel.ram)) == 0, "panel RAM differs from framebuffer");
  CHECK(panel.ram[0][0] == FONT5X7[('L' - 32) * 5], "first glyph wrong");

  panel = Ssd1306Model();
  panel.present = false;
  Oled none;
  CHECK(!none.begin(OLED_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL, OLED_I2C_HZ), "found a panel that isn't there");
  uint32_t before = panel.transactions;
  for (int i = 0; i < 20; ++i) none.service();
  CHECK(panel.transactions == before, "absent panel still gets I2C traffic");
  panel = Ssd1306Model();
}

// ====================================================================== e2e
static ZigbeeAnalog *epByNum(uint8_t n) {
  for (auto *e : Zigbee.eps) if (e->endpoint == n) return static_cast<ZigbeeAnalog *>(e);
  return nullptr;
}

static int chainBrightness() {
  int m = 0;
  for (auto &c : sim.chips) m = std::max(m, c.shown.r + c.shown.g + c.shown.b);
  return m;
}

// Each loop() iteration is 1 ms of virtual time (it ends in delay(1)); the
// microphone delivers 16 samples per millisecond.
static std::vector<uint16_t> g_tape;
static size_t g_tapePos = 0;
static void runMs(uint32_t ms) {
  for (uint32_t i = 0; i < ms; ++i) {
    if (g_tapePos < g_tape.size()) {
      size_t k = std::min<size_t>(16, g_tape.size() - g_tapePos);
      g_analyzer.feed(&g_tape[g_tapePos], k);
      g_tapePos += k;
    }
    loop();
  }
}

static void testEndToEnd() {
  printf("[e2e] boot with 3 modules + OLED, join Zigbee, endpoints 10..20\n");
  Serial.verbose = false;
  sim.nowMs = 1000;
  sim.chips.clear();
  sim.setChain(3);
  panel = Ssd1306Model();
  setup();
  CHECK(Zigbee.role == ZIGBEE_ROUTER, "should start as router");
  CHECK(Zigbee.eps.size() == 11, "expected 11 endpoints, got %zu", Zigbee.eps.size());
  CHECK(g_detected == 3, "boot scan saw %u", g_detected);
  CHECK(g_oled.present(), "OLED not detected");
  CHECK(g_buttonPinUsable, "button pin wrongly disabled");
  for (auto *e : Zigbee.eps) CHECK(e->model && strcmp(e->model, ZB_MODEL) == 0, "ep %u missing model", e->endpoint);
  // The Spooky Lights integration finds its entities by these descriptions.
  CHECK(strcmp(epByNum(EP_EFFECT)->outDesc, "Effect") == 0, "endpoint 12 description changed");
  CHECK(strcmp(epByNum(EP_COUNT)->inDesc, "Grove LEDs detected") == 0, "endpoint 11 description changed");

  Zigbee.isConnected = true;
  runMs(300);
  CHECK(epByNum(EP_SOUND_SOURCE)->reportedOut.back() == SOUND_DEFAULT_SOURCE, "sound source not published");
  CHECK(epByNum(EP_SENSITIVITY)->reportedOut.back() == SOUND_DEFAULT_SENSITIVITY, "sensitivity not published");

  printf("[e2e] Lightning Storm + a thunderclap at the mic: measure mic → light latency\n");
  epByNum(EP_EFFECT)->fromCoordinator(FX_LIGHTNING);
  zbLight.fromCoordinator(true, 254, 80, 80, 255);
  Synth syn;
  auto sig = syn.silence(1.5f);
  size_t onset = sig.size();
  Synth::add(sig, syn.thunder(3.0f, 0.35f, true), onset);
  g_tape = syn.toAdc(sig);
  g_tapePos = 0;
  // A storm soundtrack rumbles before it cracks: a faint rumble cue puts the
  // storm in "listening" mode, so it stops inventing strikes of its own.
  epByNum(EP_CUE)->fromCoordinator(encodeCue(900, CUE_RUMBLE, 100));
  runMs(1400);
  int skyBefore = 0;
  uint32_t onsetMs = 0, flashMs = 0;
  for (int i = 0; i < 300; ++i) {
    runMs(1);
    if (g_tapePos >= onset && !onsetMs) onsetMs = sim.nowMs;
    int b = chainBrightness();
    if (!onsetMs) skyBefore = std::max(skyBefore, b);
    if (onsetMs && !flashMs && b > 300 && b > skyBefore + 150) flashMs = sim.nowMs;
  }
  uint32_t latency = flashMs ? flashMs - onsetMs : 9999;
  printf("         thunder at the mic → modules lit after %u ms (analysis 8 ms + peak frame 8 ms + render ≤ 20 ms)\n",
         latency);
  CHECK(flashMs && latency <= 40, "mic → light latency %u ms", latency);
  runMs(400);
  bool scopeInk = false, scopeStrike = false;
  for (int p = 1; p <= 5; ++p)
    for (int x = 0; x < 128; ++x) scopeInk |= panel.ram[p][x] != 0;
  for (int x = 0; x < 128; ++x) scopeStrike |= panel.ram[1][x] != 0 && panel.ram[5][x] != 0;
  CHECK(scopeInk, "seismograph is empty after a thunderclap");
  CHECK(scopeStrike, "no full-height strike mark on the seismograph");
  runMs(1600);
  auto *evt = epByNum(EP_SOUND_EVENT);
  bool thunderReported = false;
  for (float v : evt->reportedIn) thunderReported |= ((int)lroundf(v) % 10) == SND_EVT_THUNDER;
  CHECK(thunderReported, "thunder event not reported to HA (%zu reports)", evt->reportedIn.size());
  CHECK(!epByNum(EP_SOUND_LEVEL)->reportedIn.empty(), "sound level never reported");

  printf("[e2e] Home Assistant cues: thunder cue flashes; ignored when the source is 'mic only'\n");
  g_tape.clear();
  runMs(3000);
  int sky = chainBrightness();
  epByNum(EP_CUE)->fromCoordinator(encodeCue(1, CUE_THUNDER, 900));
  int flash = 0;
  for (int i = 0; i < 25; ++i) { runMs(1); flash = std::max(flash, chainBrightness()); }
  CHECK(flash > sky + 200, "thunder cue didn't flash (%d → %d)", sky, flash);
  epByNum(EP_SOUND_SOURCE)->fromCoordinator(SRC_MIC);
  CHECK(snapshot().soundSource == SRC_MIC && g_bus.source() == SRC_MIC, "sound source not applied");
  runMs(3000);
  sky = chainBrightness();
  uint32_t accepted = g_bus.remoteCueCount();
  epByNum(EP_CUE)->fromCoordinator(encodeCue(2, CUE_THUNDER, 900));
  runMs(25);
  CHECK(g_bus.remoteCueCount() == accepted, "cue accepted in mic-only mode");

  printf("[e2e] sensitivity, double-click source cycling, NVS persistence\n");
  epByNum(EP_SENSITIVITY)->fromCoordinator(80);
  CHECK(g_analyzer.sensitivity() == 80, "sensitivity not applied");
  uint8_t src = snapshot().soundSource;
  uint8_t fx = snapshot().effect;
  for (int k = 0; k < 2; ++k) { sim.buttonLevel = LOW; runMs(80); sim.buttonLevel = HIGH; runMs(80); }
  runMs(500);
  CHECK(snapshot().soundSource == (src + 1) % SRC_COUNT, "double-click didn't cycle the sound source");
  CHECK(snapshot().effect == fx, "double-click also changed the effect");
  sim.bootLevel = LOW; runMs(80); sim.bootLevel = HIGH; runMs(600);
  CHECK(snapshot().effect == (fx + 1) % FX_COUNT, "BOOT-button click didn't change the effect");
  runMs(SETTINGS_SAVE_DELAY_MS + 100);
  CHECK(Preferences::kv()["src"] == snapshot().soundSource, "sound source not saved");
  CHECK(Preferences::kv()["sens"] == 80, "sensitivity not saved");

  printf("[e2e] the OLED shows the effect name\n");
  runMs(400);
  CHECK(panel.ram[0][0] != 0 || panel.ram[0][1] != 0, "effect name missing on the panel");

  printf("[e2e] no microphone: music cues alone drive the show; nothing phantom reaches HA\n");
  {
    Synth syn;
    g_tape = syn.emptyPort(4.0f, 20);
    g_tapePos = 0;
    epByNum(EP_OUTPUT)->fromCoordinator(OUT_AUTO);
    epByNum(EP_SOUND_SOURCE)->fromCoordinator(SRC_BOTH);
    epByNum(EP_EFFECT)->fromCoordinator(FX_LIGHTNING);
    size_t eventsBefore = epByNum(EP_SOUND_EVENT)->reportedIn.size();
    runMs(3000);
    CHECK(g_analyzer.presence() == 0, "empty port not detected end-to-end");
    CHECK(epByNum(EP_SOUND_EVENT)->reportedIn.size() == eventsBefore, "phantom sound events reported to HA");
    CHECK(!epByNum(EP_SOUND_LEVEL)->reportedIn.empty() && epByNum(EP_SOUND_LEVEL)->reportedIn.back() == 0,
          "sound level should drop to 0 without a mic");
    // A storm soundtrack via Sendspin: rumble then a thunderclap cue.
    epByNum(EP_CUE)->fromCoordinator(encodeCue(301, CUE_RUMBLE, 400));
    runMs(600);
    int sky = chainBrightness();
    epByNum(EP_CUE)->fromCoordinator(encodeCue(302, CUE_THUNDER, 950));
    int flash = 0;
    for (int i = 0; i < 25; ++i) { runMs(1); flash = std::max(flash, chainBrightness()); }
    printf("         sky %d → flash %d within 25 ms of the cue\n", sky, flash);
    CHECK(flash > sky + 200, "music cue didn't strike without a mic");
    bool noMicShown = false;
    for (int x = 0; x < 128; ++x) noMicShown |= panel.ram[7][x] != 0;
    CHECK(noMicShown, "OLED status line empty");
    g_tape.clear();
  }

  printf("[e2e] the orange user LED follows the show (onboard output)\n");
  epByNum(EP_OUTPUT)->fromCoordinator(OUT_ONBOARD);
  epByNum(EP_EFFECT)->fromCoordinator(FX_SOLID);
  zbLight.fromCoordinator(true, 254, 255, 128, 0);
  runMs(800);
  int expectedOn = USER_LED_ACTIVE_LOW ? 0 : 255;
  CHECK(sim.userLed == expectedOn, "user LED %d, expected %d", sim.userLed, expectedOn);
}

int main(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (strcmp(argv[i], "--trace") == 0) g_traceDir = argv[i + 1];
  Serial.verbose = false;
  testChain();
  testCueCodec();
  testPeriodicity();
  testAnalyzerQuiet();
  testAnalyzerThunder();
  testAnalyzerFire();
  testAnalyzerMusic();
  testAnalyzerScream();
  testAnalyzerSensitivity();
  testAnalyzerSweep();
  testNoMicrophone();
  testBus();
  testEffectsFuzz();
  testLightningListens();
  testCandleCrackles();
  testHeartLocks();
  testPowerLimiter();
  testOled();
  testEndToEnd();
  printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
