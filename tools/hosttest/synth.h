// Synthetic test sounds at 16 kHz — the things a Halloween porch actually hears.
// Signals are in full-scale units (1.0 = the ADC's full swing around the mic
// bias); toAdc() turns them into 12-bit samples with realistic ADC noise.
#pragma once
#include <vector>
#include <random>
#include <cmath>
#include <cstdint>
#include <algorithm>

struct Synth {
  static constexpr float FS = 16000.0f;
  std::mt19937 rng{1031};
  std::normal_distribution<float> n01{0.0f, 1.0f};
  std::uniform_real_distribution<float> u01{0.0f, 1.0f};
  float bias = 2100.0f;       // Grove mic idles near VCC/2
  float adcNoiseLsb = 2.0f;   // ESP32-C6 SAR noise, rms

  float noise() { return n01(rng); }
  float uni(float a, float b) { return a + (b - a) * u01(rng); }
  static size_t n(float sec) { return (size_t)std::lround(sec * FS); }

  static void lowpass(std::vector<float> &x, float fc, int poles) {
    float a = 1.0f - std::exp(-2.0f * (float)M_PI * fc / FS);
    for (int p = 0; p < poles; ++p) {
      float s = 0;
      for (float &v : x) { s += (v - s) * a; v = s; }
    }
  }
  // Bilinear first-order high-pass sections (unity gain at Nyquist).
  static void highpass(std::vector<float> &x, float fc, int poles) {
    float k = std::tan((float)M_PI * fc / FS);
    float b0 = 1.0f / (1.0f + k), a1 = (1.0f - k) / (1.0f + k);
    for (int p = 0; p < poles; ++p) {
      float x1 = 0, y1 = 0;
      for (float &v : x) {
        float y = b0 * (v - x1) + a1 * y1;
        x1 = v;
        y1 = y;
        v = y;
      }
    }
  }
  static void normalize(std::vector<float> &x) {
    double e = 0;
    for (float v : x) e += (double)v * v;
    float rms = (float)std::sqrt(e / std::max<size_t>(1, x.size()));
    if (rms > 0) for (float &v : x) v /= rms;
  }
  std::vector<float> whiteNoise(size_t len) {
    std::vector<float> x(len);
    for (float &v : x) v = noise();
    return x;
  }
  static void add(std::vector<float> &dst, const std::vector<float> &src, size_t at, float gain = 1.0f) {
    if (dst.size() < at + src.size()) dst.resize(at + src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); ++i) dst[at + i] += gain * src[i];
  }

  std::vector<float> silence(float sec) { return std::vector<float>(n(sec), 0.0f); }

  // A close thunderclap: a sharp broadband crack, a booming body, then a long
  // rolling rumble. smallSpeaker = played through something like an Echo Show
  // (2nd-order roll-off below 180 Hz).
  std::vector<float> thunder(float sec, float amp, bool smallSpeaker) {
    size_t len = n(sec);
    auto crack = whiteNoise(len);
    lowpass(crack, 4000, 1);
    normalize(crack);
    auto body = whiteNoise(len);
    lowpass(body, 500, 2);
    highpass(body, 150, 1);
    normalize(body);
    auto rumble = whiteNoise(len);
    lowpass(rumble, 150, 2);
    normalize(rumble);
    std::vector<float> out(len);
    float roll = uni(0, 6.28f);
    for (size_t i = 0; i < len; ++i) {
      float t = i / FS;
      float eCrack = std::exp(-t / 0.035f);
      float eBody = std::exp(-t / 0.6f);
      float eRumble = (1.0f - std::exp(-t / 0.08f)) * std::exp(-t / 1.3f) * (0.75f + 0.25f * std::sin(t * 17.0f + roll));
      out[i] = amp * (0.9f * crack[i] * eCrack + 0.5f * body[i] * eBody + 0.8f * rumble[i] * eRumble);
    }
    if (smallSpeaker) highpass(out, 180, 2);
    return out;
  }

  // A log fire: low roar, airy hiss, and crackles at `rate` per second.
  std::vector<float> fire(float sec, float amp, float rate, std::vector<float> *crackTimes = nullptr) {
    size_t len = n(sec);
    auto roar = whiteNoise(len);
    lowpass(roar, 200, 2);
    normalize(roar);
    auto hiss = whiteNoise(len);
    highpass(hiss, 1500, 1);
    normalize(hiss);
    std::vector<float> out(len);
    for (size_t i = 0; i < len; ++i) out[i] = amp * (0.04f * roar[i] + 0.015f * hiss[i]);
    std::exponential_distribution<float> gap(rate);
    for (float t = gap(rng); t < sec - 0.01f; t += gap(rng)) {
      size_t at = n(t);
      size_t dur = n(uni(0.002f, 0.004f));
      auto click = whiteNoise(dur + 40);
      highpass(click, 2500, 2);
      normalize(click);
      float peak = amp * uni(0.12f, 0.35f);
      for (size_t k = 0; k < click.size(); ++k) click[k] *= peak * std::exp(-(float)k / (dur * 0.5f + 1));
      add(out, click, at);
      if (crackTimes) crackTimes->push_back(t);
    }
    out.resize(len);
    return out;
  }

  // Something with a beat: kick drums, hi-hats on the off-beat, a soft pad.
  std::vector<float> music(float sec, float amp, float bpm, std::vector<float> *kickTimes = nullptr) {
    size_t len = n(sec);
    std::vector<float> out(len, 0.0f);
    for (size_t i = 0; i < len; ++i) {
      float t = i / FS;
      out[i] = amp * 0.05f * (std::sin(2 * M_PI * 220 * t) + std::sin(2 * M_PI * 277.2 * t) + std::sin(2 * M_PI * 329.6 * t));
    }
    float beat = 60.0f / bpm;
    for (float t = 0.1f; t < sec - 0.3f; t += beat) {
      size_t at = n(t);
      std::vector<float> kick(n(0.3f));
      float ph = 0;
      for (size_t k = 0; k < kick.size(); ++k) {
        float tt = k / FS;
        float f = 50.0f + 70.0f * std::exp(-tt / 0.03f);
        ph += 2 * (float)M_PI * f / FS;
        kick[k] = amp * 0.5f * std::sin(ph) * std::exp(-tt / 0.12f);
      }
      add(out, kick, at);
      if (kickTimes) kickTimes->push_back(t);
      auto hat = whiteNoise(n(0.05f));
      highpass(hat, 6000, 2);
      normalize(hat);
      for (size_t k = 0; k < hat.size(); ++k) hat[k] *= amp * 0.06f * std::exp(-(k / FS) / 0.015f);
      add(out, hat, n(t + beat / 2));
    }
    out.resize(len);
    return out;
  }

  // "BOO!" — a sustained, harmonic, mid-range shout.
  std::vector<float> scream(float sec, float amp) {
    size_t len = n(sec);
    std::vector<float> out(len);
    float ph = 0;
    for (size_t i = 0; i < len; ++i) {
      float t = i / FS;
      float f0 = 450.0f + 20.0f * std::sin(2 * M_PI * 6.0 * t);
      ph += 2 * (float)M_PI * f0 / FS;
      float env = std::min(1.0f, t / 0.03f) * std::min(1.0f, (sec - t) / 0.1f);
      float v = 0;
      for (int k = 1; k <= 6; ++k) v += std::sin(k * ph) / k;
      out[i] = amp * 0.5f * env * v;
    }
    return out;
  }

  // Full-scale units → 12-bit ADC samples around the bias, with ADC noise.
  std::vector<uint16_t> toAdc(const std::vector<float> &sig) {
    std::vector<uint16_t> out(sig.size());
    for (size_t i = 0; i < sig.size(); ++i) {
      float c = bias + sig[i] * 2047.0f + adcNoiseLsb * noise();
      out[i] = (uint16_t)std::clamp((int)std::lround(c), 0, 4095);
    }
    return out;
  }
};
