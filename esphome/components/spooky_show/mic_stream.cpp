#include "mic_stream.h"

#ifdef USE_ESP32
#include "esphome/core/log.h"
#include "esp_adc/adc_continuous.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

namespace esphome {
namespace spooky_show {

static const char *const TAG = "spooky_show.mic";

static adc_continuous_handle_t s_handle = nullptr;
static adc_channel_t s_channel = ADC_CHANNEL_0;
static SoundAnalyzer *s_analyzer = nullptr;
static volatile uint32_t s_overruns = 0;

static bool IRAM_ATTR on_pool_overflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  s_overruns = s_overruns + 1;
  return false;
}

// One conversion result → (channel, 12-bit value), whatever the chip.
static inline void unpack(const uint8_t *p, uint32_t *channel, uint16_t *value) {
  const adc_digi_output_data_t *d = (const adc_digi_output_data_t *) p;
#if SOC_ADC_DIGI_RESULT_BYTES == 2
  *channel = d->type1.channel;
  *value = d->type1.data;
#else
  *channel = d->type2.channel;
  *value = d->type2.data;
#endif
}

static void mic_task(void *) {
  static uint8_t buf[512];
  static uint16_t pcm[SoundAnalyzer::FRAME];
  uint16_t carry = 0;
  bool have_carry = false;
  for (;;) {
    uint32_t got = 0;
    if (adc_continuous_read(s_handle, buf, sizeof(buf), &got, 100) != ESP_OK) continue;
    size_t m = 0;
    for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
      uint32_t ch;
      uint16_t v;
      unpack(&buf[i], &ch, &v);
      if (ch != (uint32_t) s_channel) continue;
      if (!have_carry) {
        carry = v;
        have_carry = true;
        continue;
      }
      have_carry = false;
      pcm[m++] = (uint16_t) ((carry + v + 1) >> 1);   // 32 kHz → 16 kHz
      if (m == SoundAnalyzer::FRAME) {
        s_analyzer->feed(pcm, m);
        m = 0;
      }
    }
    if (m) s_analyzer->feed(pcm, m);
  }
}

bool MicStream::begin(uint8_t gpio, SoundAnalyzer *analyzer, uint32_t rate_hz) {
  s_analyzer = analyzer;
  adc_unit_t unit;
  if (adc_continuous_io_to_channel(gpio, &unit, &s_channel) != ESP_OK || unit != ADC_UNIT_1) {
    ESP_LOGE(TAG, "GPIO%u is not an ADC1 pin — the microphone needs one (GPIO32–39 on an ESP32)", gpio);
    return false;
  }
  adc_continuous_handle_cfg_t hcfg = {};
  hcfg.max_store_buf_size = 4096;
  hcfg.conv_frame_size = 512;
  if (adc_continuous_new_handle(&hcfg, &s_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Could not create the continuous ADC driver");
    return false;
  }
  adc_digi_pattern_config_t pattern = {};
  pattern.atten = 3;   // 12 dB (DB_11 in older IDFs): ~0–3.1 V
  pattern.channel = s_channel;
  pattern.unit = unit;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

  adc_continuous_config_t cfg = {};
  cfg.pattern_num = 1;
  cfg.adc_pattern = &pattern;
  cfg.sample_freq_hz = rate_hz;
  cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
#if SOC_ADC_DIGI_RESULT_BYTES == 2
  cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
#else
  cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
#endif
  if (adc_continuous_config(s_handle, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "ADC configuration rejected");
    return false;
  }
  adc_continuous_evt_cbs_t cbs = {};
  cbs.on_pool_ovf = on_pool_overflow;
  adc_continuous_register_event_callbacks(s_handle, &cbs, nullptr);
  if (adc_continuous_start(s_handle) != ESP_OK) {
    ESP_LOGE(TAG, "ADC would not start");
    return false;
  }
  // After the driver has configured the pad: an empty port now reads ~0 V.
  gpio_pullup_dis((gpio_num_t) gpio);
  gpio_pulldown_en((gpio_num_t) gpio);
  // Core 1, above ESPHome's loop task, below Wi-Fi (core 0).
  if (xTaskCreatePinnedToCore(mic_task, "spooky_mic", 4096, nullptr, 3, nullptr, portNUM_PROCESSORS - 1) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the microphone task");
    return false;
  }
  running_ = true;
  ESP_LOGI(TAG, "Listening on GPIO%u (ADC1 channel %d) at %u Hz", gpio, (int) s_channel, (unsigned) rate_hz);
  return true;
}

uint32_t MicStream::overruns() const { return s_overruns; }

}  // namespace spooky_show
}  // namespace esphome

#else  // host builds (tools/hosttest): no hardware

namespace esphome {
namespace spooky_show {
bool MicStream::begin(uint8_t, SoundAnalyzer *, uint32_t) { return false; }
uint32_t MicStream::overruns() const { return 0; }
}  // namespace spooky_show
}  // namespace esphome

#endif
