#include "MicInput.h"
#include "config.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static adc_continuous_handle_t s_handle = nullptr;
static adc_channel_t s_channel = ADC_CHANNEL_0;
static SoundAnalyzer *s_analyzer = nullptr;
static volatile uint32_t s_overruns = 0;

static bool IRAM_ATTR onPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  s_overruns = s_overruns + 1;
  return false;
}

static void micTask(void *) {
  static uint8_t buf[512];
  static uint16_t pcm[SoundAnalyzer::FRAME];
  uint16_t carry = 0;
  bool haveCarry = false;
  for (;;) {
    uint32_t got = 0;
    if (adc_continuous_read(s_handle, buf, sizeof(buf), &got, 100) != ESP_OK) continue;
    size_t m = 0;
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
      const adc_digi_output_data_t *p = (const adc_digi_output_data_t *)&buf[i];
      if (p->type2.channel != (uint32_t)s_channel) continue;
      uint16_t v = (uint16_t)p->type2.data;
      if (!haveCarry) {
        carry = v;
        haveCarry = true;
        continue;
      }
      haveCarry = false;
      pcm[m++] = (uint16_t)((carry + v + 1) >> 1);    // 32 kHz → 16 kHz
      if (m == SoundAnalyzer::FRAME) {
        s_analyzer->feed(pcm, m);
        m = 0;
      }
    }
    if (m) s_analyzer->feed(pcm, m);
  }
}

bool MicInput::begin(uint8_t gpio, SoundAnalyzer *analyzer) {
  s_analyzer = analyzer;
  adc_unit_t unit;
  if (adc_continuous_io_to_channel(gpio, &unit, &s_channel) != ESP_OK || unit != ADC_UNIT_1) {
    Serial.printf("[mic] GPIO%u is not an ADC1 pin\n", gpio);
    return false;
  }

  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = 4096;   // 32 ms of headroom
  hcfg.conv_frame_size = 512;       // 128 conversions = 4 ms per DMA frame
  if (adc_continuous_new_handle(&hcfg, &s_handle) != ESP_OK) {
    Serial.println("[mic] could not create the ADC continuous driver");
    return false;
  }

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = 3;                // 12 dB (called DB_11 in older IDFs): ~0..3.1 V
  pattern.channel = s_channel;
  pattern.unit = unit;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  adc_continuous_config_t cfg = {};
  cfg.pattern_num = 1;
  cfg.adc_pattern = &pattern;
  cfg.sample_freq_hz = MIC_ADC_RATE_HZ;
  cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if (adc_continuous_config(s_handle, &cfg) != ESP_OK) {
    Serial.println("[mic] ADC configuration rejected");
    return false;
  }

  adc_continuous_evt_cbs_t cbs = {};
  cbs.on_pool_ovf = onPoolOverflow;
  adc_continuous_register_event_callbacks(s_handle, &cbs, nullptr);

  if (adc_continuous_start(s_handle) != ESP_OK) {
    Serial.println("[mic] ADC would not start");
    return false;
  }
  if (xTaskCreate(micTask, "mic", 4096, nullptr, MIC_TASK_PRIORITY, nullptr) != pdPASS) {
    Serial.println("[mic] could not start the mic task");
    return false;
  }
  _running = true;
  Serial.printf("[mic] listening on GPIO%u (ADC1 ch%d) at %d Hz\n", gpio, (int)s_channel, MIC_ADC_RATE_HZ);
  return true;
}

uint32_t MicInput::overruns() const { return s_overruns; }

#else   // ---------------------------------------------------------------- host

bool MicInput::begin(uint8_t, SoundAnalyzer *) { return false; }
uint32_t MicInput::overruns() const { return 0; }

#endif
