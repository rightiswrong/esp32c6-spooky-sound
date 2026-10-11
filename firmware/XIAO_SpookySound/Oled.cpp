#include "Oled.h"
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "font5x7.h"

static const uint8_t INIT_SEQUENCE[] = {
  0xAE,             // display off
  0xD5, 0x80,       // clock divide / oscillator
  0xA8, 0x3F,       // multiplex 64
  0xD3, 0x00,       // no display offset
  0x40,             // start line 0
  0x8D, 0x14,       // charge pump on (internal VCC)
  0x20, 0x00,       // horizontal addressing
  0xA1,             // segment remap (column 127 = SEG0)
  0xC8,             // COM scan direction: remapped
  0xDA, 0x12,       // COM pins: alternative, no remap
  0x81, 0x8F,       // contrast (moderate: it's a dark porch)
  0xD9, 0xF1,       // pre-charge
  0xDB, 0x40,       // VCOMH deselect
  0x2E,             // no scrolling
  0xA4,             // display follows RAM
  0xA6,             // normal (not inverted)
  0xAF,             // display on
};

bool Oled::command(const uint8_t *c, size_t n) {
  Wire.beginTransmission(_addr);
  Wire.write((uint8_t)0x00);                 // control byte: commands follow
  for (size_t i = 0; i < n; ++i) Wire.write(c[i]);
  return Wire.endTransmission() == 0;
}

bool Oled::begin(uint8_t addr, uint8_t sda, uint8_t scl, uint32_t hz) {
  _addr = addr;
  Wire.begin(sda, scl);
  Wire.setClock(hz);
  Wire.beginTransmission(_addr);
  if (Wire.endTransmission() != 0) {         // nobody answered: no display
    _ok = false;
    return false;
  }
  _ok = command(INIT_SEQUENCE, sizeof(INIT_SEQUENCE));
  clear();
  return _ok;
}

void Oled::clear() { memset(_buf, 0, sizeof(_buf)); }

void Oled::pixel(int x, int y, bool on) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  uint8_t &b = _buf[(y >> 3) * WIDTH + x];
  uint8_t bit = (uint8_t)(1u << (y & 7));
  if (on) b |= bit;
  else b &= (uint8_t)~bit;
}

void Oled::vline(int x, int y0, int y1, bool on) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; ++y) pixel(x, y, on);
}

void Oled::hline(int x0, int x1, int y, bool on) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; ++x) pixel(x, y, on);
}

int Oled::text(int x, int page, const char *s, bool invert) {
  if (page < 0 || page >= PAGES) return x;
  uint8_t *row = &_buf[page * WIDTH];
  for (; *s && x <= WIDTH - 6; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) ch = '?';
    const uint8_t *g = &FONT5X7[(ch - FONT5X7_FIRST) * 5];
    for (int col = 0; col < 5; ++col) row[x + col] = invert ? (uint8_t)~g[col] : g[col];
    row[x + 5] = invert ? 0xFF : 0x00;
    x += 6;
  }
  return x;
}

bool Oled::service() {
  if (!_ok) return false;
  const uint8_t window[] = {0x21, 0x00, 0x7F, 0x22, _page, _page};   // this page, all columns
  if (!command(window, sizeof(window))) return false;
  const uint8_t *src = &_buf[_page * WIDTH];
  for (int off = 0; off < WIDTH; off += 32) {
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)0x40);               // control byte: data follows
    for (int i = 0; i < 32; ++i) Wire.write(src[off + i]);
    Wire.endTransmission();
  }
  _page = (uint8_t)((_page + 1) % PAGES);
  return _page == 0;
}
