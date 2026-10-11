// Host stand-in for the Arduino Wire (I2C) library, wired to a model of an
// SSD1306 panel so tests can check what actually reaches the display.
#pragma once
#include <stdint.h>
#include <vector>

struct Ssd1306Model {
  bool present = true;
  uint8_t addr = 0x3C;
  uint8_t ram[8][128] = {};
  int colStart = 0, colEnd = 127, pageStart = 0, pageEnd = 7, col = 0, page = 0;
  bool displayOn = false, chargePump = false;
  uint32_t transactions = 0, dataBytes = 0, nacks = 0;
  void command(const std::vector<uint8_t> &c);
  void data(const std::vector<uint8_t> &d);
};
extern Ssd1306Model panel;

class TwoWire {
public:
  bool begin(int sda = -1, int scl = -1, uint32_t freq = 0) { (void)sda; (void)scl; (void)freq; return true; }
  bool setClock(uint32_t hz) { clock = hz; return true; }
  void beginTransmission(uint8_t a) { addr = a; buf.clear(); }
  size_t write(uint8_t b) {
    if (buf.size() >= 128) return 0;          // ESP32 Wire buffer is 128 bytes
    buf.push_back(b);
    return 1;
  }
  uint8_t endTransmission(bool = true);
  uint32_t clock = 100000;
  uint8_t addr = 0;
  std::vector<uint8_t> buf;
};
extern TwoWire Wire;
