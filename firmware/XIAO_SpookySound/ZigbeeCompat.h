// =============================================================================
//  ZigbeeCompat — smooth over Arduino-ESP32 Zigbee API differences.
//
//  Core 3.3.5+ has explicit colour modes (setLightColorCapabilities +
//  onLightChangeRgb); older 3.x cores only have onLightChange. Both callbacks
//  take the same arguments. Overload resolution picks the first template when
//  the light has setLightColorCapabilities(), the fallback otherwise — no
//  version macros needed. (Lives in a header so the Arduino IDE's automatic
//  prototype generator never sees the templates.)
// =============================================================================
#pragma once

template <typename Light, typename Callback>
static auto zbAttachColorCallback(Light &light, Callback cb, int)
    -> decltype(light.setLightColorCapabilities(0), void()) {
  light.setLightColorCapabilities(1 << 3);   // ZIGBEE_COLOR_CAPABILITY_X_Y
  light.onLightChangeRgb(cb);
}

template <typename Light, typename Callback>
static void zbAttachColorCallback(Light &light, Callback cb, long) {
  light.onLightChange(cb);
}

template <typename Light, typename Callback>
static void zbAttachColorCallback(Light &light, Callback cb) {
  zbAttachColorCallback(light, cb, 0);
}
