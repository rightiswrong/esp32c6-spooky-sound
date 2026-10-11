// =============================================================================
//  Oled — a deliberately tiny SSD1306 128x64 driver for the expansion board's
//  display, built so it can never stall the light show:
//
//    * drawing only touches a 1 KB framebuffer in RAM
//    * service() pushes ONE 128-byte page (8 pixel rows) per call, in 32-byte
//      I2C chunks (~3 ms at 400 kHz); called once per 20 ms frame it refreshes
//      the whole screen about 6 times a second
//    * begin() probes the address first; with no display attached every call
//      is a cheap no-op
// =============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

class Oled {
public:
  static constexpr int WIDTH = 128;
  static constexpr int HEIGHT = 64;
  static constexpr int PAGES = HEIGHT / 8;

  bool begin(uint8_t addr, uint8_t sda, uint8_t scl, uint32_t hz);
  bool present() const { return _ok; }

  void clear();
  void pixel(int x, int y, bool on = true);
  void vline(int x, int y0, int y1, bool on = true);
  void hline(int x0, int x1, int y, bool on = true);
  // 5x7 text on a page row (8 px). Returns the x after the last character.
  int text(int x, int page, const char *s, bool invert = false);

  // Send one page to the panel. Returns true when a full frame has gone out.
  bool service();

  const uint8_t *buffer() const { return _buf; }

private:
  uint8_t _addr = 0x3C;
  bool _ok = false;
  uint8_t _page = 0;
  uint8_t _buf[WIDTH * PAGES];

  bool command(const uint8_t *c, size_t n);
};
