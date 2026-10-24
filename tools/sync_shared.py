"""Copy the shared effects / sound code into the ESPHome component.

The XIAO sketch (firmware/XIAO_SpookySound) is the canonical home of the
effects engine and the sound analysis: an Arduino sketch must contain its own
sources. The ESPHome component gets identical copies; tests/test_logic.py fails
if they drift.

    python3 tools/sync_shared.py          # copy
    python3 tools/sync_shared.py --check  # exit 1 if a copy differs
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "firmware" / "XIAO_SpookySound"
DST = ROOT / "esphome" / "components" / "spooky_show"
SHARED = (
    "Effects.h",
    "Effects.cpp",
    "SoundAnalyzer.h",
    "SoundAnalyzer.cpp",
    "SoundBus.h",
    "SoundBus.cpp",
    "P9813Chain.h",   # for RGB8 (header only)
)


def main() -> int:
    check = "--check" in sys.argv
    stale = []
    for name in SHARED:
        src, dst = (SRC / name).read_bytes(), DST / name
        if dst.exists() and dst.read_bytes() == src:
            continue
        stale.append(name)
        if not check:
            dst.write_bytes(src)
    if check and stale:
        print("out of date:", *stale, "— run python3 tools/sync_shared.py")
        return 1
    print("copied:" if stale else "in sync", *stale)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
