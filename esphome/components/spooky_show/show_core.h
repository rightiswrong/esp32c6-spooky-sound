// =============================================================================
//  ShowCore — the Halloween show for ONE analog strip (a PWM controller such as
//  the Gledopto GL-C-211WL), with ears.
//
//  The effects engine is the very same as on the XIAO and NanoC6 (shared
//  files, kept identical by tools/sync_shared.py). An analog strip shows one
//  colour at a time, so each frame the engine paints a small virtual string of
//  STRIP_PIXELS pixels and the "ceiling mixer" turns them into one colour:
//
//    coherent effects (Solid, Ghost Breath, Tell-Tale Heart, Haunted Chase,
//    Blood Moon)  →  one pixel: the wave or the passing eye, intact
//    textured effects (Candle, Pumpkin, Cauldron, Lightning, Poltergeist)
//                 →  a·mean + b·brightest: the room glow of many flames with
//                    the flare of a single crackle on top — a strobe-free way
//                    to show a crackle on a whole ceiling
//
//  Then the colour is split into the strip's channels: white extracted for
//  RGBW / RGBCCT strips, and on RGBCCT a bluish white (a lightning bolt) goes
//  to the cold-white LEDs and a reddish one to the warm-white.
//
//  No ESPHome or Arduino includes: compiled on a PC by tools/hosttest.
// =============================================================================
#pragma once
#include <stdint.h>
#include "Effects.h"
#include "SoundAnalyzer.h"
#include "SoundBus.h"

static constexpr uint8_t STRIP_PIXELS = 6;
static constexpr uint32_t STRIP_FRAME_MS = 20;     // 50 fps

enum StripMode : uint8_t { STRIP_RGB = 0, STRIP_RGBW = 1, STRIP_RGBCCT = 2 };

struct StripMix {
  float mean;      // weight of the average of all pixels
  float max;       // weight of the brightest pixel
  bool single;     // use pixel 0 only
};

// How each effect is folded onto one colour (see above).
StripMix strip_mix_for(uint8_t effect);
RGBf strip_mix(uint8_t effect, const RGBf *px, uint8_t n);

struct StripChannels {
  float r = 0, g = 0, b = 0;      // normalised so the largest is 1 (or all 0)
  float color_brightness = 0;     // scale of the RGB part, 0..1
  float white = 0;                // RGBW
  float cold_white = 0, warm_white = 0;  // RGBCCT
};
StripChannels strip_channels(RGBf c, StripMode mode);

// Microphone + remote cues → effects → one colour. One per controller.
class ShowCore : public SoundSink {
 public:
  ShowCore() { evt_mux_ = portMUX_INITIALIZER_UNLOCKED; }   // same idiom as SoundBus
  void begin(uint32_t seed);

  // Render one frame for `effect` (FX_*). `base` = the light's colour.
  RGBf render(uint8_t effect, uint32_t dt_ms, RGBf base, uint32_t now_ms);

  void set_speed(uint8_t speed) { speed_ = speed < 1 ? 1 : (speed > 100 ? 100 : speed); }
  uint8_t speed() const { return speed_; }
  SoundBus &bus() { return bus_; }
  SoundAnalyzer &analyzer() { return analyzer_; }
  const SoundAnalyzer &analyzer_const() const { return analyzer_; }
  const Effects &effects() const { return fx_; }

  // A cue by name ("thunder", "rumble", "fire", "crackle", "beat", "peak",
  // "clear"). strength 0..1; for "crackle" it is the count (1..30).
  bool cue_named(const char *name, float strength, uint32_t now_ms);

  // Last classified event, taken by the main loop (thread safe). NONE if none.
  SoundEventType take_event(float *strength);

  // SoundSink: called from the microphone task.
  void soundFrame(const SoundFeatures &f) override;
  void soundHit(const SoundHit &hit) override;
  void soundCrackle(float strength) override;
  void soundEvent(SoundEventType type, float strength) override;

  // Time source for the sink callbacks (the ESPHome wrapper sets millis()).
  void set_clock(uint32_t (*clock)()) { clock_ = clock; }

 private:
  Effects fx_;
  SoundAnalyzer analyzer_;
  SoundBus bus_;
  RGBf canvas_[STRIP_PIXELS];
  uint8_t speed_ = 50;
  uint32_t (*clock_)() = nullptr;
  portMUX_TYPE evt_mux_;
  volatile uint8_t evt_type_ = SND_EVT_NONE;
  volatile float evt_strength_ = 0;

  uint32_t now_() const { return clock_ ? clock_() : 0; }
};
