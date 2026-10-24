#include "spooky_show.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace spooky_show {

static const char *const TAG = "spooky_show";
static const char *const EVENT_NAMES[] = {"", "thunder", "fire", "loud"};

static uint32_t clock_ms() { return millis(); }

void SpookyShow::setup() {
  this->core_.set_clock(clock_ms);
  this->core_.begin(0xC0FFEEu ^ 0x31u ^ (uint32_t) random_uint32());
  if (this->has_mic_pin_ && !this->mic_.begin(this->mic_pin_, &this->core_.analyzer(), this->sample_rate_)) {
    ESP_LOGW(TAG, "Microphone not running — Home Assistant / Music Assistant cues still work");
  }
  if (this->mic_sensor_ != nullptr) this->mic_sensor_->publish_initial_state(false);
}

void SpookyShow::loop() {
  const uint32_t now = millis();

  // Microphone arrivals and departures
  int8_t presence = this->mic_.running() ? this->core_.analyzer_const().presence() : 0;
  if (presence != -1 && presence != this->presence_shown_) {
    this->presence_shown_ = presence;
    if (presence == 1) {
      ESP_LOGI(TAG, "Microphone detected — listening");
    } else if (this->has_mic_pin_) {
      ESP_LOGI(TAG, "No microphone on GPIO%u — following Home Assistant / Music Assistant cues only", this->mic_pin_);
    }
    if (this->mic_sensor_ != nullptr) this->mic_sensor_->publish_state(presence == 1);
    if (presence != 1 && this->level_sensor_ != nullptr) {
      this->level_sensor_->publish_state(0);
      this->last_level_ = 0;
    }
  }

  // Sounds heard → the "Sound heard" event (rate-limited by the analyser itself)
  float strength = 0;
  SoundEventType evt = this->core_.take_event(&strength);
  if (evt != SND_EVT_NONE && evt < SND_EVT_COUNT) {
    ESP_LOGD(TAG, "Heard: %s (%.2f)", EVENT_NAMES[evt], strength);
    if (this->sound_event_ != nullptr) this->sound_event_->trigger(EVENT_NAMES[evt]);
  }

  // Loudness: at most once a second, and only on a 3-point change
  if (this->level_sensor_ != nullptr && presence == 1 && now - this->last_level_ms_ >= 1000) {
    float dbfs = this->core_.analyzer_const().features().dbfs;
    float pct = std::round(std::fmin(100.0f, std::fmax(0.0f, (dbfs + 60.0f) * (100.0f / 60.0f))));
    if (this->last_level_ < 0 || std::fabs(pct - this->last_level_) >= 3.0f) {
      this->last_level_ms_ = now;
      this->last_level_ = pct;
      this->level_sensor_->publish_state(pct);
    }
  }
}

void SpookyShow::dump_config() {
  ESP_LOGCONFIG(TAG, "Spooky show:");
  if (this->has_mic_pin_) {
    ESP_LOGCONFIG(TAG, "  Microphone: GPIO%u at %u Hz (%s)", this->mic_pin_, (unsigned) this->sample_rate_,
                  this->mic_.running() ? "sampling" : "NOT running");
  } else {
    ESP_LOGCONFIG(TAG, "  Microphone: none configured (cues only)");
  }
  ESP_LOGCONFIG(TAG, "  Effect speed: %u", this->core_.speed());
}

// ------------------------------------------------------------------ effect
void SpookyLightEffect::apply() {
  const uint32_t now = millis();
  if (this->last_ms_ != 0 && now - this->last_ms_ < STRIP_FRAME_MS) return;
  const uint32_t dt = this->last_ms_ == 0 ? STRIP_FRAME_MS : now - this->last_ms_;
  this->last_ms_ = now;

  // remote_values = what Home Assistant asked for (effect frames don't touch it)
  const auto &remote = this->state_->remote_values;
  const float cb = remote.get_color_brightness();
  RGBf base = {remote.get_red() * cb, remote.get_green() * cb, remote.get_blue() * cb};
  if (base.r + base.g + base.b < 0.05f) base = {1.0f, 0.35f, 0.0f};   // white-only light: pumpkin orange

  RGBf c = this->show_->render(this->effect_, dt, base);

  auto traits = this->state_->get_traits();
  StripMode mode = STRIP_RGB;
  light::ColorMode cm = light::ColorMode::RGB;
  if (traits.supports_color_mode(light::ColorMode::RGB_COLD_WARM_WHITE)) {
    mode = STRIP_RGBCCT;
    cm = light::ColorMode::RGB_COLD_WARM_WHITE;
  } else if (traits.supports_color_mode(light::ColorMode::RGB_WHITE)) {
    mode = STRIP_RGBW;
    cm = light::ColorMode::RGB_WHITE;
  }
  StripChannels ch = strip_channels(c, mode);

  auto call = this->state_->make_call();
  call.set_publish(false);
  call.set_save(false);
  call.set_transition_length_if_supported(0);
  call.set_state(true);
  call.set_brightness(remote.get_brightness());
  call.set_color_mode_if_supported(cm);
  call.set_color_brightness(ch.color_brightness);
  call.set_rgb(ch.r, ch.g, ch.b);
  if (mode == STRIP_RGBW) call.set_white(ch.white);
  if (mode == STRIP_RGBCCT) {
    call.set_cold_white(ch.cold_white);
    call.set_warm_white(ch.warm_white);
  }
  call.perform();
}

}  // namespace spooky_show
}  // namespace esphome
