// Host tests for the ESPHome strip build (esphome/components/spooky_show):
// the ceiling mixer, the channel split, and the whole show — effects driven by
// synthetic audio through the real analyser, and by Music Assistant cues.
// Build & run: tools/hosttest/run.sh
#include "Arduino.h"
#include "show_core.h"
#include "synth.h"
#include <cstdio>
#include <cmath>
#include <random>

HostSerial Serial;
HostESP ESP;
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t, uint8_t) {}
int digitalRead(uint8_t) { return 0; }
void analogWrite(uint8_t, int) {}
void delayMicroseconds(uint32_t) {}
void delay(uint32_t) {}
static uint32_t g_now = 0;
uint32_t millis() { return g_now; }
static uint32_t clock_fn() { return g_now; }

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (cond) { ++g_pass; }                                       \
    else { ++g_fail; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }                   \
  } while (0)

static float lum(RGBf c) { return c.r + c.g + c.b; }

static void testChannels() {
  printf("[strip] channel split for RGB, RGBW and RGBCCT strips\n");
  StripChannels rgb = strip_channels({0.5f, 0.25f, 0.0f}, STRIP_RGB);
  CHECK(fabsf(rgb.r - 1) < 1e-6f && fabsf(rgb.g - 0.5f) < 1e-6f && fabsf(rgb.color_brightness - 0.5f) < 1e-6f,
        "RGB: %.2f %.2f %.2f cb %.2f", rgb.r, rgb.g, rgb.b, rgb.color_brightness);
  StripChannels w = strip_channels({0.9f, 0.8f, 0.7f}, STRIP_RGBW);
  CHECK(fabsf(w.white - 0.7f) < 1e-6f && fabsf(w.color_brightness - 0.2f) < 1e-6f, "RGBW white %.2f cb %.2f", w.white,
        w.color_brightness);
  StripChannels bolt = strip_channels({0.85f, 0.88f, 1.0f}, STRIP_RGBCCT);
  StripChannels warm = strip_channels({1.0f, 0.9f, 0.8f}, STRIP_RGBCCT);
  printf("         lightning bolt → cold %.2f warm %.2f;  warm white → cold %.2f warm %.2f\n", bolt.cold_white,
         bolt.warm_white, warm.cold_white, warm.warm_white);
  CHECK(bolt.cold_white > 2 * bolt.warm_white, "a bolt should be cold white");
  CHECK(warm.warm_white > 2 * warm.cold_white, "a warm white should be warm");
  CHECK(fabsf(bolt.cold_white + bolt.warm_white - 0.85f) < 1e-5f, "white energy not conserved");
  StripChannels candle = strip_channels({1.0f, 0.35f, 0.0f}, STRIP_RGBCCT);
  CHECK(candle.cold_white == 0 && candle.warm_white == 0 && candle.color_brightness == 1.0f, "candle has no white");
  StripChannels off = strip_channels({0, 0, 0}, STRIP_RGBW);
  CHECK(off.color_brightness == 0 && off.white == 0 && off.r == 0, "black");
  StripChannels over = strip_channels({1.4f, 1.4f, 1.4f}, STRIP_RGB);
  CHECK(over.color_brightness == 1.0f, "values above 1 must clamp");
}

static void testMixer() {
  printf("[strip] ceiling mixer: one crackle on six flames = a flare, not a strobe\n");
  RGBf px[STRIP_PIXELS];
  for (auto &p : px) p = {0.5f, 0.2f, 0.0f};
  RGBf calm = strip_mix(FX_CANDLE, px, STRIP_PIXELS);
  px[3] = {1.0f, 0.86f, 0.55f};
  RGBf flare = strip_mix(FX_CANDLE, px, STRIP_PIXELS);
  float jump = lum(flare) - lum(calm);
  printf("         calm %.2f → one flaring flame %.2f (+%.0f%%)\n", lum(calm), lum(flare), 100 * jump / lum(calm));
  CHECK(jump > 0.25f * lum(calm) && jump < 1.0f * lum(calm), "flare should be visible but not blinding");
  RGBf single = strip_mix(FX_GHOST_BREATH, px, STRIP_PIXELS);
  CHECK(single.r == px[0].r && single.b == px[0].b, "coherent effects show pixel 0");
  for (int e = 0; e < FX_COUNT; ++e) {
    StripMix m = strip_mix_for(e);
    CHECK(m.single || fabsf(m.mean + m.max - 1.0f) < 1e-6f, "%s weights must sum to 1", EFFECT_NAMES[e]);
  }
}

static void testEffectsAlive() {
  printf("[strip] all %d effects on one strip: finite, in range, alive (30 s each, with and without cues)\n", FX_COUNT);
  for (bool cues : {false, true}) {
    for (int e = 0; e < FX_COUNT; ++e) {
      ShowCore show;
      show.set_clock(clock_fn);
      show.begin(7);
      show.bus().setSource(SRC_BOTH);
      std::mt19937 rng(e);
      float lo = 1e9f, hi = -1e9f, prev = -1;
      int changes = 0;
      bool finite = true;
      g_now = 1000;
      for (int f = 0; f < 1500; ++f) {
        g_now += STRIP_FRAME_MS;
        if (cues && f % 40 == 0) {
          static const char *kinds[] = {"thunder", "crackle", "fire", "rumble", "beat", "peak"};
          show.cue_named(kinds[rng() % 6], (rng() % 100) / 100.0f * (rng() % 2 ? 1 : 20), g_now);
        }
        RGBf c = show.render(e, STRIP_FRAME_MS, {1.0f, 0.35f, 0.0f}, g_now);
        for (float v : {c.r, c.g, c.b}) {
          finite &= std::isfinite(v);
          lo = fminf(lo, v);
          hi = fmaxf(hi, v);
        }
        if (prev >= 0 && fabsf(lum(c) - prev) > 1e-4f) ++changes;
        prev = lum(c);
      }
      CHECK(finite, "%s produced NaN", EFFECT_NAMES[e]);
      CHECK(lo >= -1e-4f && hi <= 1.5f, "%s out of range [%f, %f]", EFFECT_NAMES[e], lo, hi);
      if (e != FX_SOLID) CHECK(changes > 10, "%s frozen (cues %d)", EFFECT_NAMES[e], cues);
    }
  }
}

// Feed `adc` to the analyser in real time alongside 50 fps rendering.
struct Run {
  ShowCore show;
  std::vector<float> lum;
  std::vector<SoundEventType> events;
  Run() {
    show.set_clock(clock_fn);
    show.begin(99);
    show.bus().setSource(SRC_BOTH);
  }
  void play(uint8_t effect, const std::vector<uint16_t> &adc, uint32_t extra_ms = 0) {
    const size_t per_frame = Synth::FS * STRIP_FRAME_MS / 1000;   // 320 samples
    size_t pos = 0;
    while (pos < adc.size() || extra_ms > 0) {
      if (pos < adc.size()) {
        size_t k = std::min(per_frame, adc.size() - pos);
        for (size_t i = 0; i < k; i += SoundAnalyzer::FRAME)
          show.analyzer().feed(&adc[pos + i], std::min<size_t>(SoundAnalyzer::FRAME, k - i));
        pos += k;
      } else {
        extra_ms = extra_ms > STRIP_FRAME_MS ? extra_ms - STRIP_FRAME_MS : 0;
      }
      g_now += STRIP_FRAME_MS;
      lum.push_back(::lum(show.render(effect, STRIP_FRAME_MS, {0.4f, 0.4f, 1.0f}, g_now)));
      float s;
      SoundEventType t = show.take_event(&s);
      if (t != SND_EVT_NONE) events.push_back(t);
    }
  }
};

static void testThunderOnTheCeiling() {
  printf("[strip] thunder through the mic → the ceiling flashes; heard as THUNDER\n");
  Synth syn;
  auto sig = syn.silence(3.0f);
  size_t onset = sig.size();
  Synth::add(sig, syn.thunder(3.0f, 0.35f, true), onset);
  Run run;
  g_now = 5000;
  // A faint storm rumble cue first: the storm listens instead of inventing strikes.
  run.show.cue_named("rumble", 0.1f, g_now);
  run.play(FX_LIGHTNING, syn.toAdc(sig));
  const size_t onsetFrame = onset / 320;
  float sky = 0;
  for (size_t i = onsetFrame - 50; i < onsetFrame; ++i) sky = fmaxf(sky, run.lum[i]);
  int flashAt = -1;
  for (size_t i = onsetFrame; i < onsetFrame + 10 && i < run.lum.size(); ++i)
    if (run.lum[i] > sky + 1.0f) { flashAt = (int) (i - onsetFrame); break; }
  printf("         sky %.2f → flash %d frame(s) after the clap (≤ %d ms)\n", sky, flashAt,
         flashAt < 0 ? -1 : (flashAt + 1) * (int) STRIP_FRAME_MS);
  CHECK(flashAt >= 0 && flashAt <= 2, "no flash within 3 frames of the thunderclap");
  bool thunder = false;
  for (auto t : run.events) thunder |= t == SND_EVT_THUNDER;
  CHECK(thunder, "thunder not reported to Home Assistant");
}

static void testFireOnTheCeiling() {
  printf("[strip] a crackling fire through the mic → a flickering glow, no lightning, FIRE heard\n");
  Synth syn;
  Run run;
  g_now = 5000;
  run.play(FX_CANDLE, syn.toAdc(syn.fire(12.0f, 0.6f, 6.0f)));
  float lo = 1e9f, hi = 0, maxJump = 0;
  for (size_t i = 200; i < run.lum.size(); ++i) {
    lo = fminf(lo, run.lum[i]);
    hi = fmaxf(hi, run.lum[i]);
    maxJump = fmaxf(maxJump, run.lum[i] - run.lum[i - 1]);
  }
  printf("         glow %.2f–%.2f, biggest frame-to-frame jump %.2f\n", lo, hi, maxJump);
  CHECK(hi - lo > 0.1f, "the fire doesn't flicker");
  CHECK(maxJump < 0.6f, "crackles strobe the ceiling (jump %.2f)", maxJump);
  bool fire = false, thunder = false;
  for (auto t : run.events) { fire |= t == SND_EVT_FIRE; thunder |= t == SND_EVT_THUNDER; }
  CHECK(fire && !thunder, "fire not recognised (fire %d thunder %d)", fire, thunder);
}

static void testCuesWithoutMic() {
  printf("[strip] no microphone: an empty port stays silent; Music Assistant cues drive the show\n");
  Synth syn;
  Run run;
  g_now = 5000;
  run.play(FX_LIGHTNING, syn.emptyPort(3.0f, 20));
  CHECK(run.show.analyzer().presence() == 0, "empty port not detected");
  CHECK(run.events.empty(), "phantom events from an empty port");
  run.show.cue_named("rumble", 0.4f, g_now);
  run.play(FX_LIGHTNING, {}, 600);
  float sky = run.lum.back();
  CHECK(run.show.cue_named("thunder", 0.95f, g_now), "thunder cue rejected");
  CHECK(!run.show.cue_named("disco", 1.0f, g_now), "unknown cue accepted");
  run.play(FX_LIGHTNING, {}, 20);
  printf("         sky %.2f → %.2f in the next frame\n", sky, run.lum.back());
  CHECK(run.lum.back() > sky + 1.0f, "thunder cue didn't strike");
  run.show.bus().setSource(SRC_MIC);
  CHECK(!run.show.cue_named("thunder", 1.0f, g_now), "cue accepted with source = microphone only");
}

int main() {
  testChannels();
  testMixer();
  testEffectsAlive();
  testThunderOnTheCeiling();
  testFireOnTheCeiling();
  testCuesWithoutMic();
  printf("\n%d strip checks passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
