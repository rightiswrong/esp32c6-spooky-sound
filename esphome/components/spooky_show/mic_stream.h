// =============================================================================
//  MicStream — continuous DMA sampling of an analog microphone (ESP-IDF).
//
//  Same pipeline as the XIAO's MicInput, for any ESP32 with an ADC1 pin:
//    ADC1 @ 32 kHz ──DMA──► mic task ──► pairs averaged (16 kHz) ──► analyser
//  Handles both result formats: TYPE1 (classic ESP32, 2 bytes per sample, as
//  in the Gledopto GL-C-211WL) and TYPE2 (C3/C6/S3, 4 bytes). The pin's
//  pull-down is enabled so an empty port reads 0 V and the analyser can tell
//  there's no microphone.
// =============================================================================
#pragma once
#include <stdint.h>
#include "SoundAnalyzer.h"

namespace esphome {
namespace spooky_show {

class MicStream {
 public:
  // Starts sampling `gpio` (must be an ADC1 pin: GPIO32–39 on a classic ESP32).
  // Returns false (and logs why) if it can't.
  bool begin(uint8_t gpio, SoundAnalyzer *analyzer, uint32_t rate_hz);
  bool running() const { return running_; }
  uint32_t overruns() const;

 private:
  bool running_ = false;
};

}  // namespace spooky_show
}  // namespace esphome
