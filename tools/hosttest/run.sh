#!/usr/bin/env bash
# Compile the firmware logic for the host against API stubs and run the tests.
# Builds twice: once against the newer colour-light API (core >= 3.3.5) and once
# against the older one, so both code paths are proven to compile.
#   tools/hosttest/run.sh                 run the tests
#   tools/hosttest/run.sh --trace DIR     also write analyser traces (CSV) to DIR
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
fw="$here/../../firmware/XIAO_SpookySound"
out="${TMPDIR:-/tmp}/spooky-sound-hosttest"
mkdir -p "$out"
flags=(-std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror
       -DZIGBEE_MODE_ZCZR -I"$here/stubs" -I"$fw" -fsanitize=address,undefined)
srcs=("$here/test_main.cpp" "$fw/P9813Chain.cpp" "$fw/Effects.cpp" "$fw/Renderer.cpp"
      "$fw/SoundAnalyzer.cpp" "$fw/SoundBus.cpp" "$fw/MicInput.cpp" "$fw/Oled.cpp")
# The ESPHome strip build: its own config.h and the shared code copies.
comp="$here/../../esphome/components/spooky_show"
echo "== build (ESPHome strip core) =="
g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -I"$here/stubs" -I"$comp" -I"$here" \
    -fsanitize=address,undefined -x c++ "$here/test_strip.cpp" "$comp/show_core.cpp" "$comp/Effects.cpp" \
    "$comp/SoundAnalyzer.cpp" "$comp/SoundBus.cpp" "$comp/mic_stream.cpp" -o "$out/test_strip" -lm
echo "== run (ESPHome strip core) =="
"$out/test_strip"

for variant in new old; do
  extra=()
  [[ $variant == new ]] && extra=(-DHOST_NEW_COLOR_API)
  echo "== build ($variant colour-light API) =="
  g++ "${flags[@]}" "${extra[@]}" -x c++ "${srcs[@]}" -o "$out/test_$variant" -lm
  echo "== run ($variant) =="
  if [[ $variant == new ]]; then "$out/test_$variant" "$@"; else "$out/test_$variant"; fi
done
