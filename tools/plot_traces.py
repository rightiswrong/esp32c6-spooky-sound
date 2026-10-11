"""Draw docs/img/how-it-hears.png from host-test analyser traces.

    tools/hosttest/run.sh --trace /tmp/traces
    python3 tools/plot_traces.py /tmp/traces

Each panel: loudness (dBFS), the rumble and crackle-rate meters, and every
event the analyser raised — booms (which may strike Lightning), other hits,
crackles, and the THUNDER / FIRE / LOUD reports sent to Home Assistant.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUT = Path(__file__).resolve().parent.parent / "docs" / "img" / "how-it-hears.png"
PANELS = [
    ("thunder_small_speaker", "Thunder through a small speaker (Echo-Show-like roll-off below 180 Hz)"),
    ("fire", "A crackling log fire, ~6 crackles/s"),
    ("music", "Music at 120 BPM: kick drums, hi-hats, a soft chord"),
    ("scream", "A trick-or-treater's scream"),
]
EVENT_NAMES = {11: "THUNDER", 12: "FIRE", 13: "LOUD"}


def load(path: Path):
    frames, events = [], []
    with path.open() as fh:
        for row in csv.DictReader(fh):
            t = float(row["t"])
            if row["dbfs"]:
                frames.append((t, float(row["dbfs"]), float(row["rumble"]), float(row["crackle_rate"])))
            elif row["event"]:
                events.append((t, int(row["event"]), float(row["strength"])))
    return frames, events


def main(trace_dir: str) -> None:
    fig, axes = plt.subplots(len(PANELS), 1, figsize=(11, 2.6 * len(PANELS)), constrained_layout=True)
    for ax, (name, title) in zip(axes, PANELS):
        frames, events = load(Path(trace_dir) / f"{name}.csv")
        t = [f[0] for f in frames]
        ax.plot(t, [f[1] for f in frames], color="#5b5b66", lw=0.8, label="loudness (dBFS)")
        ax.set_ylim(-75, 5)
        ax.set_ylabel("dBFS")
        ax.set_title(title, loc="left", fontsize=10)
        meters = ax.twinx()
        meters.plot(t, [f[2] for f in frames], color="#3a6fd8", lw=1.2, label="rumble")
        meters.plot(t, [min(1.0, f[3] / 8.0) for f in frames], color="#e08a1e", lw=1.2, label="crackle rate / 8")
        meters.set_ylim(0, 1.05)
        meters.set_ylabel("meters")
        for et, kind, _ in events:
            if kind == 3:
                ax.axvline(et, color="#7b2fbe", lw=1.4, alpha=0.9)
            elif kind == 1:
                ax.axvline(et, color="#a7a7b3", lw=0.6, ls=":")
            elif kind == 2:
                ax.plot([et], [-72], marker="|", color="#e08a1e", ms=7)
            elif kind in EVENT_NAMES:
                ax.annotate(EVENT_NAMES[kind], (et, 0), xytext=(3, -12), textcoords="offset points",
                            fontsize=8, color="#b0161b", weight="bold")
                ax.axvline(et, color="#b0161b", lw=1.0, ls="--")
    handles = [
        plt.Line2D([], [], color="#5b5b66", lw=0.8, label="loudness (dBFS)"),
        plt.Line2D([], [], color="#3a6fd8", lw=1.2, label="rumble meter"),
        plt.Line2D([], [], color="#e08a1e", lw=1.2, label="crackle rate / 8"),
        plt.Line2D([], [], color="#7b2fbe", lw=1.4, label="BOOM (can strike Lightning)"),
        plt.Line2D([], [], color="#a7a7b3", lw=0.6, ls=":", label="other hit"),
        plt.Line2D([], [], color="#e08a1e", marker="|", ls="", ms=7, label="crackle"),
        plt.Line2D([], [], color="#b0161b", lw=1.0, ls="--", label="event sent to Home Assistant"),
    ]
    axes[0].legend(handles=handles, loc="upper right", fontsize=7, ncol=4)
    axes[-1].set_xlabel("seconds")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=110)
    print("wrote", OUT)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/traces")
