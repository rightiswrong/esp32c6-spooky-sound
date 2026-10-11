"""Unit tests for the HA-free logic module, plus firmware ⇄ integration sync checks.

Runs with the standard library only:  python3 -m unittest discover -s tests -v
"""

from __future__ import annotations

import importlib.util
import re
import struct
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware" / "XIAO_SpookySound"
_spec = importlib.util.spec_from_file_location("spooky_sound_logic", ROOT / "custom_components" / "spooky_sound" / "logic.py")
logic = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = logic          # dataclasses need the module registered
_spec.loader.exec_module(logic)

IEEE = "00:12:4b:00:2a:3b:4c:5d"


class FirmwareSyncTests(unittest.TestCase):
    """If someone edits the firmware, these fail until the integration follows."""

    def test_effect_names_match_firmware(self):
        src = (FW / "Effects.cpp").read_text()
        block = re.search(r"EFFECT_NAMES\[FX_COUNT\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
        self.assertEqual(tuple(re.findall(r'"((?:[^"\\]|\\.)*)"', block)), logic.EFFECTS)

    def test_endpoints_match_firmware(self):
        cfg = (FW / "config.h").read_text()
        eps = {k: int(v) for k, v in re.findall(r"#define EP_(\w+)\s+(\d+)", cfg)}
        self.assertEqual(
            {
                logic.ROLE_LIGHT: eps["LIGHT"],
                logic.ROLE_EFFECT: eps["EFFECT"],
                logic.ROLE_SOURCE: eps["SOUND_SOURCE"],
                logic.ROLE_SENSITIVITY: eps["SENSITIVITY"],
                logic.ROLE_CUE: eps["CUE"],
                logic.ROLE_LEVEL: eps["SOUND_LEVEL"],
                logic.ROLE_EVENT: eps["SOUND_EVENT"],
            },
            logic.ROLE_ENDPOINTS,
        )

    def test_cue_types_match_firmware(self):
        src = (FW / "SoundBus.h").read_text()
        fw = {name.lower(): int(v) for name, v in re.findall(r"CUE_(\w+)\s*=\s*(\d+)", src) if name != "NONE"}
        self.assertEqual(fw, logic.CUE_TYPES)

    def test_event_types_match_firmware(self):
        src = (FW / "SoundAnalyzer.h").read_text()
        fw = {int(v): name.lower() for name, v in re.findall(r"SND_EVT_(\w+)\s*=\s*(\d+)", src) if name not in ("NONE",)}
        self.assertEqual(fw, logic.EVENT_TYPES)

    def test_sound_source_order_matches_firmware(self):
        src = (FW / "SoundBus.h").read_text()
        order = re.findall(r"SRC_(OFF|MIC|REMOTE|BOTH)\s*=\s*(\d)", src)
        names = [n for n, _ in sorted(order, key=lambda t: int(t[1]))]
        self.assertEqual(names, ["OFF", "MIC", "REMOTE", "BOTH"])
        self.assertEqual(len(logic.SOUND_SOURCES), 4)

    def test_cue_formula_matches_firmware(self):
        src = (FW / "SoundBus.cpp").read_text()
        self.assertIn("seq * 10000u + (uint32_t)type * 1000u + param", src)
        self.assertEqual(logic.encode_cue(7, "thunder", 800), 71800)

    def test_endpoint_descriptions_are_recognised(self):
        """The descriptions the firmware gives ZHA must map to the right roles."""
        ino = (FW / "XIAO_SpookySound.ino").read_text()
        wanted = {
            "Sound source": logic.ROLE_SOURCE,
            "Mic sensitivity": logic.ROLE_SENSITIVITY,
            "Sound cue": logic.ROLE_CUE,
            "Sound level": logic.ROLE_LEVEL,
            "Sound event": logic.ROLE_EVENT,
            "Effect": logic.ROLE_EFFECT,
        }
        for desc, role in wanted.items():
            found = re.search(r'"(' + re.escape(desc) + r'[^"]*)"', ino)
            self.assertIsNotNone(found, desc)
            domain = logic.ROLE_DOMAINS[role]
            self.assertEqual(logic.guess_role(domain, f"{IEEE}-99-13", found.group(1)), role, desc)


class CueTests(unittest.TestCase):
    def test_roundtrip_through_float32(self):
        for seq in (0, 1, 500, 999):
            for kind, code in logic.CUE_TYPES.items():
                for param in (0, 1, 500, 999):
                    v = logic.encode_cue(seq, kind, param)
                    f32 = struct.unpack("<f", struct.pack("<f", float(v)))[0]
                    self.assertEqual(logic.decode_cue(f32), (seq, code, param))

    def test_clamps(self):
        self.assertEqual(logic.encode_cue(1, "fire", 5000), 13999)
        self.assertEqual(logic.encode_cue(1000, "fire", 1), 2 * 10000 + 3000 + 1)
        self.assertIsNone(logic.decode_cue(-1))
        self.assertIsNone(logic.decode_cue(float("nan")))
        with self.assertRaises(KeyError):
            logic.encode_cue(1, "disco", 1)

    def test_sequence_never_zero(self):
        seq = logic.SequenceCounter()
        values = [seq.next() for _ in range(2500)]
        self.assertNotIn(0, values)
        self.assertEqual(values[998], 999)
        self.assertEqual(values[999], 1)

    def test_events(self):
        self.assertEqual(logic.decode_event("41.0"), (4, "thunder"))
        self.assertEqual(logic.decode_event(52), (5, "fire"))
        self.assertEqual(logic.decode_event("123"), (12, "loud"))
        self.assertIsNone(logic.decode_event("0"))
        self.assertIsNone(logic.decode_event("unavailable"))
        self.assertIsNone(logic.decode_event("17"))

    def test_token_bucket(self):
        bucket = logic.TokenBucket(rate=5, burst=3)
        self.assertEqual([bucket.take(0.0) for _ in range(4)], [True, True, True, False])
        self.assertTrue(bucket.take(0.2))          # one token back after 0.2 s
        self.assertFalse(bucket.take(0.2))
        self.assertFalse(bucket.take(0.4, reserve=1))   # low priority keeps a spare token
        self.assertTrue(bucket.take(0.4))


class RoleTests(unittest.TestCase):
    def test_zha_names(self):
        cases = [
            ("light", f"{IEEE}-10", "Light", logic.ROLE_LIGHT),
            ("number", f"{IEEE}-12-13", "Effect", logic.ROLE_EFFECT),
            ("number", f"{IEEE}-16-13", "Sound source (0 off,1 mic,2 remote,3 both)", logic.ROLE_SOURCE),
            ("number", f"{IEEE}-17-13", "Mic sensitivity", logic.ROLE_SENSITIVITY),
            ("number", f"{IEEE}-18-13", "Sound cue", logic.ROLE_CUE),
            ("sensor", f"{IEEE}-19-12", "Sound level", logic.ROLE_LEVEL),
            ("sensor", f"{IEEE}-20-12", "Sound event", logic.ROLE_EVENT),
        ]
        for domain, uid, name, want in cases:
            with self.subTest(name=name):
                self.assertEqual(logic.guess_role(domain, uid, name), want)

    def test_other_spooky_numbers_are_not_the_effect(self):
        for name in ("Effect speed", "Spotlight pixel (0 = off)", "Output (0 auto,1 onboard,2 grove,3 both)",
                     "Manual LED count (0 = auto)"):
            self.assertNotEqual(logic.guess_role("number", f"{IEEE}-13-13", name), logic.ROLE_EFFECT, name)

    def test_endpoint_fallback(self):
        self.assertEqual(logic.guess_role("number", f"{IEEE}-18-13", None), logic.ROLE_CUE)
        self.assertEqual(logic.guess_role("sensor", f"{IEEE}-20-12", None), logic.ROLE_EVENT)
        self.assertEqual(logic.guess_role("number", f"{IEEE}-12-13", None), logic.ROLE_EFFECT)
        self.assertIsNone(logic.guess_role("number", f"{IEEE}-13-13", None))

    def test_zigbee2mqtt_property_names(self):
        base = "0x00124b002a3b4c5d"
        cases = [
            ("number", f"{base}_sound_source_sound_zigbee2mqtt", logic.ROLE_SOURCE),
            ("number", f"{base}_mic_sensitivity_sound_zigbee2mqtt", logic.ROLE_SENSITIVITY),
            ("number", f"{base}_sound_cue_cue_zigbee2mqtt", logic.ROLE_CUE),
            ("sensor", f"{base}_sound_level_level_zigbee2mqtt", logic.ROLE_LEVEL),
            ("sensor", f"{base}_sound_event_event_zigbee2mqtt", logic.ROLE_EVENT),
            ("number", f"{base}_effect_index_effect_zigbee2mqtt", logic.ROLE_EFFECT),
        ]
        for domain, uid, want in cases:
            with self.subTest(uid=uid):
                self.assertEqual(logic.guess_role(domain, uid), want)

    def test_z2m_converter_uses_known_names(self):
        mjs = (ROOT / "zigbee2mqtt" / "xiao_spooky_sound.mjs").read_text()
        for prop in ("sound_source", "mic_sensitivity", "sound_cue", "sound_level", "sound_event", "effect_index"):
            self.assertIn(f'"{prop}"', mjs)
        self.assertIn('"XIAO-C6-SpookySound"', mjs)


class MoodTests(unittest.TestCase):
    def test_titles(self):
        cases = {
            ("Thunderstorm Sounds for Sleep", "Nature Sounds", None): "storm",
            ("Crackling Fireplace", None, "Cozy Winter"): "fire",
            ("Heartbeat (Slow)", "SFX", None): "heartbeat",
            ("The Witch's Brew", None, None): "cauldron",
            ("Haunted Mansion Ambience", None, None): "haunting",
            ("Thunderstorm by the Fireplace", None, None): "storm",   # storms win
            ("Firestarter", "The Prodigy", None): None,               # whole words only
            ("Lose Yourself", "Eminem", None): None,
        }
        for fields, want in cases.items():
            with self.subTest(fields=fields):
                self.assertEqual(logic.match_mood(fields), want)

    def test_custom_keywords(self):
        kw = {"fire": logic.parse_keywords("yule log, hygge")}
        self.assertEqual(logic.match_mood(["Yule Log 10 hours"], kw), "fire")
        self.assertIsNone(logic.match_mood(["Crackling Fireplace"], kw))   # replaced, not extended

    def test_every_mood_targets_a_real_effect(self):
        for mood in logic.MOODS:
            self.assertIn(mood.effect, logic.EFFECTS)
        for effect in logic.EFFECT_FOCUS:
            self.assertIn(effect, logic.EFFECTS)

    def test_focus(self):
        self.assertEqual(logic.focus_for_effect(3), logic.FOCUS_STORM)
        self.assertEqual(logic.focus_for_effect(8), logic.FOCUS_FIRE)
        self.assertIsNone(logic.focus_for_effect(0))
        self.assertIsNone(logic.focus_for_effect(None))


@dataclass
class Frame:
    timestamp_us: int
    loudness: int | None = None
    spectrum: list | None = None
    peak_strength: int | None = None
    is_downbeat: bool | None = None


def spectrum_db(low_db: float, mid_db: float, high_db: float) -> list[int]:
    """16 log bins (40 Hz–8 kHz): bins 0–5 low, 6–11 mid, 12–15 high."""
    def enc(db):
        return max(0, min(65535, round((db + 60.0) / 60.0 * 65535)))
    return [enc(low_db)] * 6 + [enc(mid_db)] * 6 + [enc(high_db)] * 4


class VisualizerTests(unittest.TestCase):
    def test_bin_split(self):
        edges = logic.bin_edges()
        self.assertAlmostEqual(edges[0], 40)
        self.assertAlmostEqual(edges[-1], 8000)
        low = [i for i in range(16) if edges[i + 1] <= 315]
        high = [i for i in range(16) if edges[i] >= 1900]
        self.assertEqual(low, [0, 1, 2, 3, 4, 5])
        self.assertEqual(high, [12, 13, 14, 15])
        bassy = logic.spectrum_shares(spectrum_db(-10, -40, -50))
        hissy = logic.spectrum_shares(spectrum_db(-50, -40, -10))
        self.assertGreater(bassy.low, 0.9)
        self.assertGreater(hissy.high, 0.9)

    def test_storm_thunder_and_rumble(self):
        eng = logic.VisualizerCueEngine(focus=logic.FOCUS_STORM)
        cues = []
        t = 0
        for _ in range(60):                               # 2 s of quiet sky
            cues += eng.feed(Frame(t, spectrum=spectrum_db(-50, -45, -55)))
            t += 33_333
        cues += eng.feed(Frame(t, peak_strength=230))     # the crack
        for _ in range(60):                               # then a big rumble
            cues += eng.feed(Frame(t, spectrum=spectrum_db(-12, -35, -50)))
            t += 33_333
        kinds = [c.kind for c in cues]
        self.assertIn("thunder", kinds)
        thunder = next(c for c in cues if c.kind == "thunder")
        self.assertGreater(thunder.param, 850)
        rumbles = [c.param for c in cues if c.kind == "rumble"]
        self.assertTrue(rumbles and max(rumbles) > 500, rumbles)
        self.assertLess(len(rumbles), 15, "rumble cues must be throttled")

    def test_fire_crackles_bunch_and_level(self):
        eng = logic.VisualizerCueEngine(focus=logic.FOCUS_FIRE)
        cues = []
        t = 0
        for i in range(90):                               # 3 s at 30 fps
            cues += eng.feed(Frame(t, loudness=40000, spectrum=spectrum_db(-30, -32, -20)))
            if i % 3 == 0:
                cues += eng.feed(Frame(t + 1000, peak_strength=90))   # ~10 crackles/s
            t += 33_333
        crackle_cues = [c for c in cues if c.kind == "crackle"]
        self.assertGreaterEqual(sum(c.param for c in crackle_cues), 25)
        self.assertLessEqual(len(crackle_cues), 14, "crackles should be bunched ~4 per second")
        fire = [c for c in cues if c.kind == "fire"]
        self.assertTrue(fire)
        self.assertLessEqual(len(fire), 3)
        self.assertNotIn("thunder", [c.kind for c in cues])

    def test_beats(self):
        eng = logic.VisualizerCueEngine(focus=logic.FOCUS_BEAT)
        cues = eng.feed(Frame(0, is_downbeat=True)) + eng.feed(Frame(500_000, is_downbeat=False))
        self.assertEqual([(c.kind, c.param) for c in cues], [("beat", 999), ("beat", 800)])

    def test_beat_fallback_from_bassy_peaks(self):
        eng = logic.VisualizerCueEngine(focus=logic.FOCUS_BEAT)
        eng.feed(Frame(0, spectrum=spectrum_db(-10, -40, -50)))
        cues = eng.feed(Frame(10_000, peak_strength=200))
        self.assertEqual([c.kind for c in cues], ["beat"])

    def test_no_focus_no_cues(self):
        eng = logic.VisualizerCueEngine(focus=None)
        out = []
        for i in range(30):
            out += eng.feed(Frame(i * 33_333, loudness=60000, spectrum=spectrum_db(-5, -5, -5), peak_strength=255))
        self.assertEqual(out, [])

    def test_bang_focus(self):
        eng = logic.VisualizerCueEngine(focus=logic.FOCUS_BANG)
        self.assertEqual([c.kind for c in eng.feed(Frame(0, peak_strength=200))], ["peak"])
        self.assertEqual(eng.feed(Frame(1, peak_strength=20)), [])

    def test_scheduling(self):
        # Audio plays 1.5 s from now; Zigbee needs 120 ms → write in 1.38 s.
        self.assertAlmostEqual(logic.cue_delay_s(2_500_000, 1_000_000, 120), 1.38)
        self.assertLess(logic.cue_delay_s(1_050_000, 1_000_000, 120), 0)

    def test_sendspin_url(self):
        self.assertEqual(logic.sendspin_url("192.168.1.10"), "ws://192.168.1.10:8927/sendspin")
        self.assertEqual(logic.sendspin_url("http://192.168.1.10:8095"), "ws://192.168.1.10:8927/sendspin")
        self.assertEqual(logic.sendspin_url("mass.local:8095/"), "ws://mass.local:8927/sendspin")
        self.assertEqual(logic.sendspin_url("ws://x:1234/custom"), "ws://x:1234/custom")
        self.assertEqual(logic.sendspin_url("http://[fd00::5]:8095"), "ws://[fd00::5]:8927/sendspin")
        self.assertIsNone(logic.sendspin_url(""))


class PackagingTests(unittest.TestCase):
    def test_versions_agree(self):
        import json

        manifest = json.loads((ROOT / "custom_components" / "spooky_sound" / "manifest.json").read_text())
        cfg = (FW / "config.h").read_text()
        fw_version = re.search(r'#define FW_VERSION "([^"]+)"', cfg).group(1)
        self.assertEqual(manifest["version"], fw_version)

    def test_translations_cover_selects_and_events(self):
        import json

        en = json.loads((ROOT / "custom_components" / "spooky_sound" / "translations" / "en.json").read_text())
        states = en["entity"]["select"]["sound_source"]["state"]
        self.assertEqual(set(states), set(logic.SOUND_SOURCES))
        ev = en["entity"]["event"]["sound_event"]["state_attributes"]["event_type"]["state"]
        self.assertEqual(set(ev), set(logic.EVENT_TYPES.values()))
        moods = en["entity"]["sensor"]["soundtrack_mood"]["state"]
        self.assertEqual(set(moods), set(logic.MOOD_KEYS) | {"none"})


if __name__ == "__main__":
    unittest.main()
