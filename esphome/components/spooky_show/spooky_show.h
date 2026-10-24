// =============================================================================
//  spooky_show — ESPHome component: the Spooky Lights Halloween effects, with
//  ears, on a PWM (analog strip) light such as the Gledopto GL-C-211WL.
//
//    spooky_show:          the show: microphone, sound bus, effects engine
//    light: effects:
//      - spooky: ...       one entry per effect (SpookyLightEffect)
//
//  The light keeps being a normal Home Assistant light: its colour is the
//  effects' base colour, its brightness their brightness, and the effect list
//  shows the Halloween effects by name.
// =============================================================================
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/light/light_effect.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/event/event.h"

#include "show_core.h"
#include "mic_stream.h"

namespace esphome {
namespace spooky_show {

class SpookyShow : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // --- configuration (codegen)
  void set_microphone_pin(uint8_t pin) { mic_pin_ = pin; has_mic_pin_ = true; }
  void set_sample_rate(uint32_t hz) { sample_rate_ = hz; }
  void set_level_sensor(sensor::Sensor *s) { level_sensor_ = s; }
  void set_microphone_sensor(binary_sensor::BinarySensor *s) { mic_sensor_ = s; }
  void set_sound_event(event::Event *e) { sound_event_ = e; }

  // --- runtime controls (lambdas in YAML: template select / numbers, API actions)
  void set_speed(float speed) { core_.set_speed((uint8_t) speed); }
  // 0 off, 1 microphone, 2 remote (Home Assistant / Music Assistant), 3 both
  void set_sound_source(uint8_t source) { core_.bus().setSource(source); }
  void set_sensitivity(float s) { core_.analyzer().setSensitivity((uint8_t) s); }
  // A raw cue as written by the Spooky Sound integration (seq·10000 + type·1000 + param)
  void cue(float value) { core_.bus().remoteCue(value, millis()); }
  // A cue by name: thunder, rumble, fire, crackle, beat, peak, clear
  void cue_named(const std::string &name, float strength) { core_.cue_named(name.c_str(), strength, millis()); }

  // --- used by SpookyLightEffect
  RGBf render(uint8_t effect, uint32_t dt_ms, RGBf base) { return core_.render(effect, dt_ms, base, millis()); }
  bool microphone_present() const { return mic_.running() && core_.analyzer_const().micPresent(); }

 protected:
  ShowCore core_;
  MicStream mic_;
  uint8_t mic_pin_ = 0;
  bool has_mic_pin_ = false;
  uint32_t sample_rate_ = 32000;
  sensor::Sensor *level_sensor_ = nullptr;
  binary_sensor::BinarySensor *mic_sensor_ = nullptr;
  event::Event *sound_event_ = nullptr;
  int8_t presence_shown_ = -2;
  uint32_t last_level_ms_ = 0;
  float last_level_ = -1;
};

class SpookyLightEffect : public light::LightEffect {
 public:
  SpookyLightEffect(const char *name, SpookyShow *show, uint8_t effect) : LightEffect(name), show_(show), effect_(effect) {}
  void start() override { this->last_ms_ = 0; }
  void apply() override;

 protected:
  SpookyShow *show_;
  uint8_t effect_;
  uint32_t last_ms_ = 0;
};

}  // namespace spooky_show
}  // namespace esphome
