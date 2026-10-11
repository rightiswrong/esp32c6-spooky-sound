// Bit-level simulation of the hardware the firmware talks to:
//   * a chain of P9813 chips clocked on CI rising edges
//   * the shared DI / loopback-sense node (10k series + 18k pull-down)
//   * the buttons, the orange user LED and an SSD1306 panel on I2C
//   * millis()/delay() on a virtual clock
#pragma once
#include "Arduino.h"
#include <vector>
#include <deque>
#include <assert.h>
#include "Wire.h"
#include "../../firmware/XIAO_SpookySound/config.h"
#include "../../firmware/XIAO_SpookySound/P9813Chain.h"

enum class ChipModel { Consume, ShiftRegister };

struct SimChip {
  ChipModel model = ChipModel::Consume;
  int latency = 1;          // re-timing delay in clocks (0 or 1)
  // consume model
  enum { ARMED, CONSUMING, FORWARDING } st = FORWARDING;
  int zeros = 0, bits = 0;
  uint32_t shift = 0, latched = 0;
  bool pending = false, prev = false;
  RGB8 shown{0, 0, 0};
  // shift-register model
  std::deque<bool> fifo;

  bool clock(bool b) {
    if (model == ChipModel::ShiftRegister) {
      if (fifo.empty()) fifo.assign(32 + latency, false);
      fifo.push_back(b);
      bool o = fifo.front();
      fifo.pop_front();
      return o;
    }
    bool out, consumed = false;
    zeros = b ? 0 : zeros + 1;
    if (st == CONSUMING) {
      consumed = true;
      shift = (shift << 1) | b;
      if (++bits == 32) { latched = shift; pending = true; st = FORWARDING; }
      out = false;
    } else if (st == ARMED && b) {
      st = CONSUMING; shift = 1; bits = 1; out = false; consumed = true;
    } else {
      out = latency ? prev : b;
    }
    if (zeros >= 32) {
      if (pending) {
        // word layout is flag | B | G | R  → R is the low byte
        shown = {(uint8_t)(latched & 0xFF), (uint8_t)(latched >> 8), (uint8_t)(latched >> 16)};
        pending = false;
      }
      st = ARMED;
    }
    prev = consumed ? false : b;   // a chip never forwards its own word
    return out;
  }
};

struct Sim {
  std::vector<SimChip> chips;
  bool loopback = true;
  bool stuckHigh = false;
  uint8_t dataMode = OUTPUT, dataLevel = LOW, clkLevel = LOW;
  bool lastDO = false;
  uint64_t clockEdges = 0;
  uint32_t nowMs = 0;
  uint64_t nowUs = 0;
  int userLed = -1;          // last analogWrite() to the orange LED
  int userLedWrites = 0;
  uint8_t buttonLevel = HIGH;     // expansion-board button (GPIO1)
  uint8_t bootLevel = HIGH;       // XIAO BOOT button (GPIO9)

  void setChain(int n, ChipModel m = ChipModel::Consume, int latency = 1) {
    // Plugging/unplugging: keep existing chips' state, add fresh ones at the end
    std::vector<SimChip> next(n);
    for (int i = 0; i < n; ++i) {
      if (i < (int)chips.size()) next[i] = chips[i];
      next[i].model = m;
      next[i].latency = latency;
    }
    chips.swap(next);
    lastDO = false;
  }
  void risingEdge() {
    assert(dataMode == OUTPUT && "CI rose while DI was released — module #1 would sample the loopback!");
    bool b = dataLevel;
    for (auto &c : chips) b = c.clock(b);
    lastDO = chips.empty() ? false : b;
    ++clockEdges;
  }
  int senseLevel() const {
    if (stuckHigh) return HIGH;
    if (dataMode == OUTPUT) return dataLevel;
    return (loopback && !chips.empty() && lastDO) ? HIGH : LOW;   // 18k pull-down otherwise
  }
};
extern Sim sim;

#ifdef SIM_IMPLEMENTATION
Sim sim;
HostSerial Serial;
HostESP ESP;
Ssd1306Model panel;
TwoWire Wire;
void pinMode(uint8_t pin, uint8_t mode) { if (pin == PIN_GROVE_DATA) sim.dataMode = mode; }
void digitalWrite(uint8_t pin, uint8_t val) {
  if (pin == PIN_GROVE_DATA) sim.dataLevel = val;
  if (pin == PIN_GROVE_CLK) {
    if (val && !sim.clkLevel) sim.risingEdge();
    sim.clkLevel = val;
  }
}
int digitalRead(uint8_t pin) {
  if (pin == PIN_GROVE_DATA) return sim.senseLevel();
  if (pin == PIN_BUTTON) return sim.buttonLevel;
  if (pin == PIN_BOOT_BUTTON) return sim.bootLevel;
  return LOW;
}
void analogWrite(uint8_t pin, int value) {
  if (pin == PIN_USER_LED) { sim.userLed = value; sim.userLedWrites++; }
}
void delayMicroseconds(uint32_t us) { sim.nowUs += us; }
void delay(uint32_t ms) { sim.nowMs += ms; }
uint32_t millis() { return sim.nowMs; }

void Ssd1306Model::command(const std::vector<uint8_t> &c) {
  for (size_t i = 0; i < c.size(); ++i) {
    uint8_t b = c[i];
    if (b == 0x21 && i + 2 < c.size()) { colStart = c[i + 1]; colEnd = c[i + 2]; col = colStart; i += 2; }
    else if (b == 0x22 && i + 2 < c.size()) { pageStart = c[i + 1]; pageEnd = c[i + 2]; page = pageStart; i += 2; }
    else if (b == 0x8D && i + 1 < c.size()) { chargePump = c[i + 1] == 0x14; i += 1; }
    else if (b == 0xAF) displayOn = true;
    else if (b == 0xAE) displayOn = false;
    else if (b == 0xD5 || b == 0xA8 || b == 0xD3 || b == 0x20 || b == 0xDA || b == 0x81 || b == 0xD9 || b == 0xDB) i += 1;
  }
}
void Ssd1306Model::data(const std::vector<uint8_t> &d) {
  for (uint8_t b : d) {                       // horizontal addressing mode
    ram[page & 7][col & 127] = b;
    dataBytes++;
    if (++col > colEnd) { col = colStart; if (++page > pageEnd) page = pageStart; }
  }
}
uint8_t TwoWire::endTransmission(bool) {
  panel.transactions++;
  if (!panel.present || addr != panel.addr) { panel.nacks++; return 2; }   // NACK on address
  if (!buf.empty()) {
    std::vector<uint8_t> rest(buf.begin() + 1, buf.end());
    if (buf[0] == 0x00) panel.command(rest);
    else if (buf[0] == 0x40) panel.data(rest);
  }
  return 0;
}
#endif
