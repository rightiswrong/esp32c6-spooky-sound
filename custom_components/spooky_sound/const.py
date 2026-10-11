"""Constants for Spooky Sound."""

from __future__ import annotations

from typing import Final

from homeassistant.const import Platform

from .logic import (  # noqa: F401  (re-exported)
    CUE_TYPES,
    EFFECTS,
    EVENT_TYPES,
    MOOD_KEYS,
    MOODS,
    ROLE_CUE,
    ROLE_DOMAINS,
    ROLE_EFFECT,
    ROLE_EVENT,
    ROLE_LEVEL,
    ROLE_LIGHT,
    ROLE_SENSITIVITY,
    ROLE_SOURCE,
    SOUND_SOURCES,
)

DOMAIN: Final = "spooky_sound"
MANUFACTURER: Final = "SpookyLab"
MODEL: Final = "XIAO-C6-SpookySound"

CONF_DEVICE_ID: Final = "device_id"
CONF_MEDIA_PLAYER: Final = "media_player"
CONF_SENDSPIN: Final = "sendspin"
CONF_SENDSPIN_HOST: Final = "sendspin_host"
CONF_LEAD_MS: Final = "lead_ms"
CONF_KEYWORDS_PREFIX: Final = "keywords_"

DEFAULT_LEAD_MS: Final = 120
RESTORE_GRACE_S: Final = 15        # keep the soundtrack's effect through short pauses

# aiosendspin is only installed if the Sendspin bridge is switched on.
SENDSPIN_REQUIREMENT: Final = "aiosendspin>=10.0.0,<11"
SENDSPIN_CLIENT_NAME: Final = "Spooky Sound"

# Zigbee budget: never more than ~6 cues/s to one light (bursts of 4).
CUE_RATE: Final = 6.0
CUE_BURST: Final = 4.0

PLATFORMS: Final = [
    Platform.BUTTON,
    Platform.EVENT,
    Platform.NUMBER,
    Platform.SELECT,
    Platform.SENSOR,
    Platform.SWITCH,
]

EVENT_SOUND_DETECTED: Final = f"{DOMAIN}_detected"
SERVICE_SEND_CUE: Final = "send_cue"


def signal_update(entry_id: str) -> str:
    """Dispatcher signal: mood / bridge status changed."""
    return f"{DOMAIN}_{entry_id}_update"
