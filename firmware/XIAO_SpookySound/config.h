// =============================================================================
//  XIAO Spooky Sound — hardware & behaviour configuration
//  Target: Seeed Studio XIAO ESP32-C6 on the Seeed "Expansion Board Base for
//  XIAO", Arduino-ESP32 core 3.x
// =============================================================================
#pragma once
#include <stdint.h>

// Keep in sync with custom_components/spooky_sound/manifest.json "version"
// (the release workflow refuses to publish if they differ from the tag).
#define FW_VERSION "0.1.0"

// ---------------------------------------------------------------- Board pins
// XIAO ESP32-C6 pin map (arduino-esp32 variants/XIAO_ESP32C6/pins_arduino.h):
//   D0 = GPIO0 (A0)   D1 = GPIO1   D2 = GPIO2   D3 = GPIO21
//   D4 = GPIO22 SDA   D5 = GPIO23 SCL   D6 = GPIO16 TX   D7 = GPIO17 RX
//   LED_BUILTIN = GPIO15 (orange)   BOOT button = GPIO9
//   GPIO3 = RF switch enable, GPIO14 = antenna select (LOW = on-board ceramic)
//
// Expansion board Grove ports used:
//   A0/D0 port ... Grove Analog Microphone   yellow = A0 (GPIO0, ADC1 ch0)
//   UART port .... Grove Chainable RGB LED   yellow = D7 (GPIO17) -> P9813 CI
//                                             white  = D6 (GPIO16) -> P9813 DI
//   The I2C ports are left alone: the OLED and RTC on the board share that bus,
//   and bit-banged LED data on it would scribble on the display.
//
// If your string stays dark, your UART port may be wired the other way round:
// swap the two numbers below.
#define PIN_MIC              0
#define PIN_GROVE_CLK        17
#define PIN_GROVE_DATA       16
#define PIN_BUTTON           1    // expansion-board user button (D1), active low
#define PIN_BOOT_BUTTON      9    // the XIAO's own BOOT button works too
#define PIN_USER_LED         15   // XIAO orange user LED (stands in for "onboard")
#define USER_LED_ACTIVE_LOW  1    // set 0 if the orange LED is lit when it should be dark
#define PIN_BUZZER           21   // D3 = expansion-board buzzer, held silent
#define PIN_SD_CS            2    // D2 = SD card chip select, held high (idle)
#define PIN_I2C_SDA          22
#define PIN_I2C_SCL          23
#define PIN_RF_SWITCH_EN     3
#define PIN_RF_ANT_SELECT    14
#define USE_EXTERNAL_ANTENNA 0    // 1 = use the u.FL connector instead of the chip antenna

// ---------------------------------------------------------- Grove P9813 chain
// Hard upper bound on chained modules. Each module can draw ~60 mA at full
// white. The power limiter below keeps the whole string inside the budget.
#define MAX_CHAIN            24

// Half clock period in microseconds. 3 us (~150 kHz) is calm and reliable for
// hand-made Halloween cable runs. Raise it if far-away modules flicker.
#define P9813_HALF_PERIOD_US 3

// Settling time before sampling the loopback sense line (see docs/WIRING.md).
#define P9813_SENSE_SETTLE_US 6

// Chain hot-plug detection
//   1 = LOOPBACK : the last module's OUT data line comes back to the DI pin
//                  through a divider; modules are counted live.
//   0 = OFF      : no return wire; set the count from Home Assistant instead.
#define CHAIN_DETECT_LOOPBACK 1
#define CHAIN_PROBE_INTERVAL_MS 1500
#define CHAIN_DEBOUNCE_PROBES 2

// ---------------------------------------------------------------- Rendering
#define FRAME_INTERVAL_MS    20      // 50 fps
#define FADE_MS              450     // on/off fade length
#define GAMMA                2.2f

// Supply budget for the LEDs. The XIAO's 3V3 regulator is not a LED supply:
// feed the chain from the expansion board's 5 V pin (USB 5 V) as described in
// docs/WIRING.md, then 1000 mA is fine on a 2 A USB supply. If you run the
// chain from the Grove port's own VCC, drop this to ~350.
#define POWER_BUDGET_MA      1000
#define MA_PER_CHAIN_CHANNEL 20
#define MA_PER_ONBOARD_CHANNEL 3     // the orange user LED is tiny

// -------------------------------------------------------------------- Sound
#define MIC_ENABLED          1
// ADC runs at 32 kHz; pairs of samples are averaged (a crude anti-alias
// filter) so the analyser works at 16 kHz in 8 ms frames of 128 samples.
#define MIC_ADC_RATE_HZ      32000
#define MIC_TASK_PRIORITY    3       // above loop() (1), below the Zigbee task (5)

// Sound reactivity default (changeable from Home Assistant or a double-click):
//   0 off · 1 microphone · 2 remote (Home Assistant / Music Assistant) · 3 both
#define SOUND_DEFAULT_SOURCE 3
#define SOUND_DEFAULT_SENSITIVITY 50   // 1..100, 50 = normal

// Remote "continuous" cues (fire level, rumble) fade out if Home Assistant
// stops refreshing them — e.g. the track ended or HA restarted.
#define SOUND_REMOTE_HOLD_MS 4000

// How long the effects stay in "listening" mode after the last sound event:
// e.g. Lightning Storm stops making its own random strikes and waits for
// the soundtrack's thunder instead.
#define SOUND_LISTEN_MS      20000

// Report the sound level to HA at most this often (and on >= 3 % change).
#define SOUND_LEVEL_REPORT_MS 1000
// Never report more than one detected event per this many ms.
#define SOUND_EVENT_REPORT_MS 250

// -------------------------------------------------------------------- OLED
#define OLED_ENABLED         1       // auto-detected at boot; harmless if absent
#define OLED_I2C_ADDR        0x3C
#define OLED_I2C_HZ          400000

// ------------------------------------------------------------------- Zigbee
#define ZB_MANUFACTURER      "SpookyLab"
#define ZB_MODEL             "XIAO-C6-SpookySound"

// Endpoints 10..15 are identical to NanoC6 Spooky Lights, so the Spooky Lights
// Home Assistant integration drives this light unchanged.
#define EP_LIGHT             10   // Color dimmable light (on/off, level, color)
#define EP_COUNT             11   // Analog In: detected LEDs / Analog Out: manual count
#define EP_EFFECT            12   // Analog Out: effect index
#define EP_SPEED             13   // Analog Out: effect speed 1..100
#define EP_OUTPUT            14   // Analog Out: output routing mode
#define EP_SPOTLIGHT         15   // Analog Out: spotlight pixel (0 = off)
// New in Spooky Sound:
#define EP_SOUND_SOURCE      16   // Analog Out: 0 off, 1 mic, 2 remote, 3 both
#define EP_SENSITIVITY       17   // Analog Out: mic sensitivity 1..100
#define EP_CUE               18   // Analog Out: sound cue written by Home Assistant
#define EP_SOUND_LEVEL       19   // Analog In: loudness 0..100 (-60..0 dBFS)
#define EP_SOUND_EVENT       20   // Analog In: last detected event (seq*10 + type)

// ------------------------------------------------------------------ Button
#define BUTTON_FACTORY_RESET_MS 5000
#define BUTTON_DOUBLE_CLICK_MS  350
#define SETTINGS_SAVE_DELAY_MS  5000
