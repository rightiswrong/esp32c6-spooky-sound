"""Pure logic for Spooky Sound — no Home Assistant imports, so it is unit-testable anywhere.

Everything that has to agree with the firmware lives here (cue encoding, event
codes, endpoint numbers, effect names), and tests/test_logic.py reads the
firmware sources to prove that it still does.
"""

from __future__ import annotations

import math
import re
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Iterable, Protocol

# --------------------------------------------------------------- firmware protocol

# Index == value of the firmware's "Effect" analog output (endpoint 12).
EFFECTS: tuple[str, ...] = (
    "Solid",
    "Candle Flicker",
    "Ghost Breath",
    "Lightning Storm",
    "Witch's Cauldron",
    "Tell-Tale Heart",
    "Haunted Chase",
    "Poltergeist",
    "Possessed Pumpkin",
    "Blood Moon",
)

# Index == value of the firmware's "Sound source" analog output (endpoint 16).
SOUND_SOURCES: tuple[str, ...] = ("off", "microphone", "remote", "both")

# Firmware SoundEventType (endpoint 20 value = seq * 10 + type).
EVENT_TYPES: dict[int, str] = {1: "thunder", 2: "fire", 3: "loud"}

# Firmware CueType (endpoint 18 value = seq * 10000 + type * 1000 + param).
CUE_TYPES: dict[str, int] = {
    "thunder": 1,
    "rumble": 2,
    "fire": 3,
    "crackle": 4,
    "beat": 5,
    "peak": 6,
    "clear": 9,
}
CUE_MAX = 9_999_999

# ------------------------------------------------------------------- entity roles

ROLE_LIGHT = "light"
ROLE_EFFECT = "effect_number"
ROLE_SOURCE = "source_number"
ROLE_SENSITIVITY = "sensitivity_number"
ROLE_CUE = "cue_number"
ROLE_LEVEL = "level_sensor"
ROLE_EVENT = "event_sensor"

ROLE_DOMAINS: dict[str, str] = {
    ROLE_LIGHT: "light",
    ROLE_EFFECT: "number",
    ROLE_SOURCE: "number",
    ROLE_SENSITIVITY: "number",
    ROLE_CUE: "number",
    ROLE_LEVEL: "sensor",
    ROLE_EVENT: "sensor",
}

# Other domains a role may be mapped to. The effect role can be a light whose
# effect list holds the Halloween effects by name (the ESPHome strip firmware),
# instead of the Zigbee firmware's "Effect" number.
ROLE_EXTRA_DOMAINS: dict[str, tuple[str, ...]] = {ROLE_EFFECT: ("light",)}


def role_domains(role: str) -> list[str]:
    return [ROLE_DOMAINS[role], *ROLE_EXTRA_DOMAINS.get(role, ())]


def is_spooky_effect_list(effects) -> bool:
    """Does a light's effect list carry the Halloween effects (ESPHome strip)?"""
    return bool(effects) and sum(1 for name in EFFECTS if name in effects) >= 5


def effect_index_from_name(name: str | None) -> int:
    """A light's current effect → index, or -1 for none / not one of ours."""
    return EFFECTS.index(name) if name in EFFECTS else -1


# Firmware endpoint for each role (config.h EP_*)
ROLE_ENDPOINTS: dict[str, int] = {
    ROLE_LIGHT: 10,
    ROLE_EFFECT: 12,
    ROLE_SOURCE: 16,
    ROLE_SENSITIVITY: 17,
    ROLE_CUE: 18,
    ROLE_LEVEL: 19,
    ROLE_EVENT: 20,
}

# Keywords: ZHA names entities from the ZCL "description" the firmware sets
# (read once, at pairing); its unique_ids are "<ieee>-<endpoint>-<cluster>",
# with "-analog_input" appended for sensors. The best-effort Zigbee2MQTT
# converter in /contrib/zigbee2mqtt uses these property names.
ROLE_KEYWORDS: dict[str, tuple[str, ...]] = {
    ROLE_SOURCE: ("sound_source",),
    ROLE_SENSITIVITY: ("sensitivity",),
    ROLE_CUE: ("sound_cue",),
    ROLE_LEVEL: ("sound_level",),
    ROLE_EVENT: ("sound_event",),
    ROLE_EFFECT: ("effect_index", "effect"),
}
# Specific roles first: "Effect speed" must not be taken for "Effect".
_KEYWORD_PRIORITY = (ROLE_SOURCE, ROLE_SENSITIVITY, ROLE_CUE, ROLE_LEVEL, ROLE_EVENT, ROLE_EFFECT)
_NOT_EFFECT = ("speed", "spotlight", "output", "manual", "count", "detected")

_EP_PATTERNS = (
    re.compile(r"[-_](1\d|20)(?:[-_]|$)"),  # ZHA: "<ieee>-16-13"
    re.compile(r"(?:^|[-_])ep_?(1\d|20)(?:[-_]|$)"),
)


def to_number(state: Any) -> float | None:
    """Parse a HA state string into a float (None for unknown/unavailable/garbage)."""
    if state is None:
        return None
    try:
        value = float(state)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def to_index(state: Any, upper: int, default: int = 0) -> int:
    """Parse a numeric state and clamp it into 0..upper."""
    value = to_number(state)
    if value is None:
        return default
    return max(0, min(upper, round(value)))


def endpoint_from_unique_id(unique_id: str) -> int | None:
    for pattern in _EP_PATTERNS:
        if match := pattern.search(unique_id.lower()):
            return int(match.group(1))
    return None


def guess_role(domain: str, unique_id: str, *names: str | None) -> str | None:
    """Guess which role an entity plays from its domain, unique_id and names."""
    candidates = [role for role, dom in ROLE_DOMAINS.items() if dom == domain]
    if not candidates:
        return None
    if domain == "light":
        return ROLE_LIGHT
    text = " ".join(n for n in (unique_id, *names) if n).lower().replace(" ", "_")
    for role in _KEYWORD_PRIORITY:
        if role not in candidates:
            continue
        if role == ROLE_EFFECT and any(word in text for word in _NOT_EFFECT):
            continue
        if any(k in text for k in ROLE_KEYWORDS[role]):
            return role
    endpoint = endpoint_from_unique_id(unique_id)
    if endpoint is not None:
        matches = [r for r in candidates if ROLE_ENDPOINTS[r] == endpoint]
        if len(matches) == 1:
            return matches[0]
    return None


# ------------------------------------------------------------------ cues & events


def encode_cue(seq: int, cue: str | int, param: int) -> int:
    """value = seq * 10000 + type * 1000 + param (mirrors firmware encodeCue)."""
    code = CUE_TYPES[cue] if isinstance(cue, str) else int(cue)
    if not 0 <= code <= 9:
        raise ValueError(f"bad cue type {cue!r}")
    seq = int(seq) % 999 + 1 if seq > 999 else max(0, int(seq))
    param = max(0, min(999, int(param)))
    return seq * 10000 + code * 1000 + param


def decode_cue(value: float) -> tuple[int, int, int] | None:
    if value is None or not math.isfinite(value) or value < 0 or value > CUE_MAX:
        return None
    v = round(value)
    return v // 10000, (v // 1000) % 10, v % 1000


def strength_param(strength: float) -> int:
    """0..1 → 0..999 (the firmware divides by 999)."""
    return max(0, min(999, round(float(strength) * 999)))


def decode_event(value: Any) -> tuple[int, str] | None:
    """Endpoint 20 value → (seq, event type) or None."""
    number = to_number(value)
    if number is None or number <= 0:
        return None
    v = round(number)
    kind = EVENT_TYPES.get(v % 10)
    return (v // 10, kind) if kind else None


class SequenceCounter:
    """1..999, wrapping; 0 is reserved for "no sequence"."""

    def __init__(self) -> None:
        self._seq = 0

    def next(self) -> int:
        self._seq = self._seq % 999 + 1
        return self._seq


class TokenBucket:
    """Rate limiter for the Zigbee mesh: `rate` cues/s, bursts up to `burst`."""

    def __init__(self, rate: float, burst: float) -> None:
        self.rate = rate
        self.burst = burst
        self._tokens = burst
        self._at: float | None = None

    def take(self, now: float, reserve: float = 0.0) -> bool:
        """Spend a token if one is available above `reserve` (lower priority when > 0)."""
        if self._at is not None:
            self._tokens = min(self.burst, self._tokens + (now - self._at) * self.rate)
        self._at = now
        if self._tokens - 1.0 >= reserve - 1e-9:
            self._tokens -= 1.0
            return True
        return False


# ------------------------------------------------------------------- moods

@dataclass(frozen=True)
class Mood:
    key: str
    effect: str
    keywords: tuple[str, ...]


MOODS: tuple[Mood, ...] = (
    Mood("storm", "Lightning Storm", ("thunder", "thunderstorm", "storm", "lightning", "tempest", "monsoon")),
    Mood("fire", "Candle Flicker", ("fireplace", "fire", "hearth", "crackling", "campfire", "bonfire", "candle")),
    Mood("heartbeat", "Tell-Tale Heart", ("heartbeat", "heart beat", "tell-tale", "telltale", "pulse")),
    Mood("cauldron", "Witch's Cauldron", ("cauldron", "witch", "potion", "bubbling", "brew")),
    Mood("haunting", "Poltergeist", ("haunted", "ghost", "poltergeist", "scream", "screams", "creepy", "spooky")),
)
MOOD_KEYS: tuple[str, ...] = tuple(m.key for m in MOODS)
MOODS_BY_KEY: dict[str, Mood] = {m.key: m for m in MOODS}


def parse_keywords(text: str | Iterable[str] | None) -> tuple[str, ...]:
    """'thunder, storm , rain' → ('thunder', 'storm', 'rain')."""
    if text is None:
        return ()
    parts = text.split(",") if isinstance(text, str) else list(text)
    return tuple(p.strip().lower() for p in parts if p and p.strip())


def match_mood(fields: Iterable[str | None], keywords: dict[str, tuple[str, ...]] | None = None) -> str | None:
    """First mood whose keyword appears as a whole word in any of the fields.

    Order matters: a "Thunderstorm by the fireplace" track is a storm first.
    """
    text = " ".join(f for f in fields if f).lower()
    if not text:
        return None
    for mood in MOODS:
        words = (keywords or {}).get(mood.key) or mood.keywords
        for word in words:
            if re.search(r"(?<![a-z0-9])" + re.escape(word) + r"(?![a-z0-9])", text):
                return mood.key
    return None


# Which kind of cues an effect wants (anything else would waste airtime).
FOCUS_STORM = "storm"
FOCUS_FIRE = "fire"
FOCUS_BEAT = "beat"
FOCUS_BANG = "bang"
EFFECT_FOCUS: dict[str, str] = {
    "Lightning Storm": FOCUS_STORM,
    "Candle Flicker": FOCUS_FIRE,
    "Possessed Pumpkin": FOCUS_FIRE,
    "Tell-Tale Heart": FOCUS_BEAT,
    "Poltergeist": FOCUS_BANG,
    "Haunted Chase": FOCUS_BANG,
    "Witch's Cauldron": FOCUS_BANG,
}


def focus_for_effect(effect_index: int | None) -> str | None:
    if effect_index is None or not 0 <= effect_index < len(EFFECTS):
        return None
    return EFFECT_FOCUS.get(EFFECTS[effect_index])


# --------------------------------------------------------- Sendspin visualizer → cues
#
# Music Assistant's Sendspin server streams "visualizer" frames time-stamped to
# the moment the audio plays: loudness, a dB-scaled spectrum, energy-onset
# "peak" events and (with Smart Fades analysis) beats. They arrive ahead of
# the audio, so cues can be sent early enough to cancel the Zigbee delay.

SPECTRUM_BINS = 16
SPECTRUM_F_MIN = 40
SPECTRUM_F_MAX = 8000


class VisualizerFrame(Protocol):
    timestamp_us: int
    loudness: int | None
    spectrum: list[int] | None
    peak_strength: int | None
    is_downbeat: bool | None


def bin_edges(n: int = SPECTRUM_BINS, f_min: float = SPECTRUM_F_MIN, f_max: float = SPECTRUM_F_MAX) -> list[float]:
    """Edges of n log-spaced bins (the 'log' scale of the Sendspin spec)."""
    ratio = f_max / f_min
    return [f_min * ratio ** (i / n) for i in range(n + 1)]


def db_to_power(v: int) -> float:
    """Sendspin uint16 (0 = -60 dB, 65535 = 0 dB) → linear power."""
    db = v / 65535.0 * 60.0 - 60.0
    return 10.0 ** (db / 10.0)


@dataclass
class SpectrumShares:
    low: float
    high: float
    power: float


def spectrum_shares(bins: list[int], edges: list[float] | None = None) -> SpectrumShares:
    """Share of power below 300 Hz and above 2 kHz (same split as the firmware)."""
    edges = edges or bin_edges(len(bins))
    powers = [db_to_power(v) for v in bins]
    total = sum(powers) or 1e-12
    low = sum(p for p, hi in zip(powers, edges[1:]) if hi <= 300.0 * 1.05)
    high = sum(p for p, lo in zip(powers, edges[:-1]) if lo >= 2000.0 * 0.95)
    return SpectrumShares(low / total, high / total, total)


@dataclass
class Cue:
    timestamp_us: int
    kind: str
    param: int


@dataclass
class VisualizerCueEngine:
    """Turns visualizer frames into the handful of cues the current effect needs.

    Pure: feed() frames, get Cue objects back (with the play-time they belong
    to). The caller schedules and rate-limits the Zigbee writes.
    """

    focus: str | None = None
    edges: list[float] = field(default_factory=bin_edges)
    _spectra: deque = field(default_factory=lambda: deque(maxlen=12))
    _level: float = 0.0
    _low_sm: float = 0.0
    _low_floor: float = 0.0
    _rumble: float = 0.0
    _last_rumble_sent: float = -1.0
    _last_rumble_ts: int = 0
    _last_fire_sent: float = -1.0
    _last_fire_ts: int = 0
    _crackles: int = 0
    _crackle_ts: int = 0
    _last_beat_ts: int | None = None

    def set_focus(self, focus: str | None) -> None:
        self.focus = focus

    def _shares_near(self, ts: int) -> SpectrumShares | None:
        best = None
        for frame_ts, shares in self._spectra:
            if abs(frame_ts - ts) <= 60_000 and (best is None or abs(frame_ts - ts) < abs(best[0] - ts)):
                best = (frame_ts, shares)
        return best[1] if best else None

    def feed(self, frame: VisualizerFrame) -> list[Cue]:
        out: list[Cue] = []
        ts = int(frame.timestamp_us)
        focus = self.focus

        if frame.spectrum:
            shares = spectrum_shares(list(frame.spectrum), self.edges if len(self.edges) == len(frame.spectrum) + 1 else None)
            self._spectra.append((ts, shares))
            low_power = shares.low * shares.power
            self._low_sm += (low_power - self._low_sm) * 0.3
            if self._low_floor <= 0:
                self._low_floor = self._low_sm
            elif self._low_sm < self._low_floor:
                self._low_floor += (self._low_sm - self._low_floor) * 0.03
            else:
                self._low_floor += (self._low_sm - self._low_floor) * 0.004
            ratio = self._low_sm / max(self._low_floor, 1e-9)
            target = max(0.0, min(1.0, math.log2(max(ratio, 1.0)) / 4.0)) if shares.low >= 0.2 else 0.0
            self._rumble += (target - self._rumble) * 0.2
            if focus == FOCUS_STORM and (
                abs(self._rumble - self._last_rumble_sent) >= 0.1
                or (self._rumble > 0.05 and ts - self._last_rumble_ts >= 2_000_000)
            ):
                self._last_rumble_sent = self._rumble
                self._last_rumble_ts = ts
                out.append(Cue(ts, "rumble", strength_param(self._rumble)))

        if frame.loudness is not None:
            self._level = max(0.0, min(1.0, (frame.loudness / 65535.0 - 0.35) / 0.5))
            if focus == FOCUS_FIRE and (
                abs(self._level - self._last_fire_sent) >= 0.1
                or ts - self._last_fire_ts >= 2_000_000
            ):
                self._last_fire_sent = self._level
                self._last_fire_ts = ts
                out.append(Cue(ts, "fire", strength_param(self._level)))

        if frame.peak_strength is not None:
            strength = frame.peak_strength / 255.0
            shares = self._shares_near(ts)
            if focus == FOCUS_STORM and strength >= 0.3:
                out.append(Cue(ts, "thunder", strength_param(strength)))
            elif focus == FOCUS_FIRE and (shares is None or shares.high >= 0.25 or strength < 0.6):
                if self._crackles == 0:
                    self._crackle_ts = ts
                self._crackles += 1
            elif focus == FOCUS_BANG and strength >= 0.4:
                out.append(Cue(ts, "peak", strength_param(strength)))
            elif (
                focus == FOCUS_BEAT
                and (self._last_beat_ts is None or ts - self._last_beat_ts > 4_000_000)   # no real beats lately
                and shares is not None
                and shares.low >= 0.3
                and strength >= 0.3
            ):
                out.append(Cue(ts, "beat", strength_param(strength)))

        if frame.is_downbeat is not None and focus == FOCUS_BEAT:
            self._last_beat_ts = ts
            out.append(Cue(ts, "beat", 999 if frame.is_downbeat else 800))

        # Crackles go out in bunches (the firmware spreads them over ~0.4 s).
        if self._crackles and ts - self._crackle_ts >= 250_000:
            out.append(Cue(self._crackle_ts, "crackle", min(30, self._crackles)))
            self._crackles = 0
        return out


def cue_delay_s(play_time_us: int, now_us: int, lead_ms: float) -> float:
    """Seconds from now until a cue for `play_time_us` should be written to Zigbee.

    Negative means "already late": send immediately (or drop if very late).
    """
    return (play_time_us - now_us) / 1_000_000.0 - lead_ms / 1000.0


def sendspin_url(host: str | None, port: int = 8927) -> str | None:
    """Music Assistant's Sendspin endpoint for third-party clients.

    Accepts 'mass.local', '192.168.1.10', 'http://192.168.1.10:8095' (the MA
    integration's URL — its web port is not the Sendspin port) or a full
    ws:// URL, which is used as given.
    """
    if not host:
        return None
    host = host.strip()
    if host.startswith(("ws://", "wss://")):
        return host
    host = re.sub(r"^[a-z]+://", "", host).split("/")[0]
    if host.startswith("["):  # IPv6 literal
        bare = host.split("]")[0] + "]"
    else:
        bare = host.split(":")[0]
    return f"ws://{bare}:{port}/sendspin" if bare else None
