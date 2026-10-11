// =============================================================================
//  MicInput — continuous DMA sampling of the Grove Analog Microphone.
//
//  The Arduino analogContinuous() helper only returns per-frame averages,
//  which would throw away exactly the transients we listen for. So this uses
//  the ESP-IDF continuous ADC driver directly:
//
//    ADC1 @ 32 kHz ──DMA──► 512-byte conversion frames ──► mic task
//      mic task: keep our channel, average sample pairs (→ 16 kHz, a gentle
//      anti-alias filter that also halves the ADC noise power) and feed the
//      SoundAnalyzer in 128-sample chunks (one 8 ms analysis frame).
//
//  The task runs above loop() and below the Zigbee stack, so a long LED probe
//  frame never makes the microphone drop samples.
//
//  The pin's pull-down is enabled after the ADC starts, so an empty port reads
//  0 V and SoundAnalyzer can report the microphone as absent.
//
//  On a host build begin() just returns false; tests feed the analyser
//  directly with synthetic audio.
// =============================================================================
#pragma once
#include <stdint.h>
#include "SoundAnalyzer.h"

class MicInput {
public:
  bool begin(uint8_t gpio, SoundAnalyzer *analyzer);
  bool running() const { return _running; }
  uint32_t overruns() const;      // DMA pool overflows (should stay 0)

private:
  bool _running = false;
};
