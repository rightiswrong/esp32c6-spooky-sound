"""Spooky Sound — let a Halloween light hear the room and follow the soundtrack.

Works on top of the Zigbee entities that ZHA / Zigbee2MQTT create for the
XIAO Spooky Sound light (it also enhances a NanoC6 Spooky Lights light, which
has no microphone, by switching its effect to match what's playing):

* "Sound source" select and a "Sound heard" event (thunder / fire / scream)
* follows a Music Assistant (or any) media player: a thunderstorm track turns
  on Lightning Storm, a crackling fireplace Candle Flicker, …
* optional Sendspin bridge: joins Music Assistant as a visualizer and sends
  the light timed cues — the strike lands with the thunderclap
* a `spooky_sound.send_cue` action for your own automations
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall, State, callback
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.typing import ConfigType

from .const import (
    CONF_DEVICE_ID,
    CONF_KEYWORDS_PREFIX,
    CONF_LEAD_MS,
    CONF_MEDIA_PLAYER,
    CONF_SENDSPIN,
    CONF_SENDSPIN_HOST,
    CUE_BURST,
    CUE_RATE,
    CUE_TYPES,
    DEFAULT_LEAD_MS,
    DOMAIN,
    EFFECTS,
    MANUFACTURER,
    MODEL,
    MOOD_KEYS,
    PLATFORMS,
    ROLE_CUE,
    ROLE_DOMAINS,
    ROLE_EFFECT,
    SERVICE_SEND_CUE,
    signal_update,
)
from .logic import (
    MOODS_BY_KEY,
    SequenceCounter,
    TokenBucket,
    encode_cue,
    focus_for_effect,
    parse_keywords,
    strength_param,
    to_index,
)

_LOGGER = logging.getLogger(__name__)

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

type SoundConfigEntry = ConfigEntry[SoundRuntime]

SEND_CUE_SCHEMA = vol.Schema(
    {
        vol.Optional("device_id"): vol.Any(cv.string, [cv.string]),
        vol.Required("cue"): vol.In(list(CUE_TYPES)),
        vol.Optional("strength", default=0.8): vol.All(vol.Coerce(float), vol.Range(min=0, max=1)),
        vol.Optional("count", default=6): vol.All(vol.Coerce(int), vol.Range(min=1, max=30)),
    }
)


@dataclass
class SoundRuntime:
    """Per-entry runtime data shared by all platforms."""

    hass: HomeAssistant
    entry_id: str
    roles: dict[str, str]
    device_info: DeviceInfo
    device_id: str | None
    media_player: str | None = None
    keywords: dict[str, tuple[str, ...]] = field(default_factory=dict)
    sendspin_enabled: bool = False
    sendspin_host: str | None = None
    lead_ms: float = DEFAULT_LEAD_MS
    follow_music: bool = True
    music_sync: bool = True
    mood: str | None = None
    bridge_status: str = "off"
    cues_sent: int = 0
    cues_dropped: int = 0
    seq: SequenceCounter = field(default_factory=SequenceCounter)
    bucket: TokenBucket = field(default_factory=lambda: TokenBucket(CUE_RATE, CUE_BURST))
    follower: Any = None
    bridge: Any = None

    # -------------------------------------------------------------- entities
    def entity(self, role: str) -> str | None:
        return self.roles.get(role) or None

    def state(self, role: str) -> State | None:
        entity_id = self.entity(role)
        return self.hass.states.get(entity_id) if entity_id else None

    def effect_index(self) -> int | None:
        state = self.state(ROLE_EFFECT)
        if state is None or state.state in ("unknown", "unavailable"):
            return None
        return to_index(state.state, len(EFFECTS) - 1)

    def focus(self) -> str | None:
        """Which cues the light wants right now (from its effect, else the mood)."""
        if (focus := focus_for_effect(self.effect_index())) is not None:
            return focus
        if self.mood and (mood := MOODS_BY_KEY.get(self.mood)):
            return focus_for_effect(EFFECTS.index(mood.effect))
        return None

    @callback
    def notify(self) -> None:
        async_dispatcher_send(self.hass, signal_update(self.entry_id))

    async def async_set_number(self, role: str, value: float, *, blocking: bool = True) -> None:
        """Write a value to one of the firmware's Analog Output endpoints."""
        entity_id = self.entity(role)
        if not entity_id:
            return
        await self.hass.services.async_call(
            "number", "set_value", {"entity_id": entity_id, "value": value}, blocking=blocking
        )

    async def async_send_cue(self, kind: str, param: int, *, priority: bool = False) -> bool:
        """Send one cue to the light, within the Zigbee budget.

        Low-priority cues (continuous levels) keep a token in reserve so they
        never crowd out a thunderclap.
        """
        if not self.entity(ROLE_CUE):
            return False
        now = self.hass.loop.time()
        if not self.bucket.take(now, reserve=0.0 if priority else 1.0):
            self.cues_dropped += 1
            return False
        value = encode_cue(self.seq.next(), kind, param)
        self.cues_sent += 1
        # Fire and forget: waiting for the Zigbee round trip would only add
        # jitter to the next scheduled cue.
        await self.async_set_number(ROLE_CUE, value, blocking=False)
        return True


def _build_device_info(hass: HomeAssistant, entry: ConfigEntry) -> tuple[DeviceInfo, str | None]:
    """Attach our entities to the existing ZHA/Z2M device when we know it."""
    device_id = entry.data.get(CONF_DEVICE_ID)
    if device_id and (device := dr.async_get(hass).async_get(device_id)):
        if device.identifiers:
            return DeviceInfo(identifiers=set(device.identifiers)), device_id
        if device.connections:
            return DeviceInfo(connections=set(device.connections)), device_id
    return (
        DeviceInfo(identifiers={(DOMAIN, entry.entry_id)}, manufacturer=MANUFACTURER, model=MODEL, name=entry.title),
        None,
    )


def merged_config(entry: ConfigEntry) -> dict[str, Any]:
    """Options override data, so re-mapping things doesn't need a re-add."""
    return {**entry.data, **entry.options}


def roles_from_config(conf: dict[str, Any]) -> dict[str, str]:
    return {role: conf[role] for role in ROLE_DOMAINS if conf.get(role)}


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Register the send_cue action (once, for all entries)."""

    async def _send_cue(call: ServiceCall) -> None:
        data = SEND_CUE_SCHEMA(dict(call.data))
        wanted = data.get("device_id")
        if isinstance(wanted, str):
            wanted = [wanted]
        targets = [
            e.runtime_data
            for e in hass.config_entries.async_loaded_entries(DOMAIN)
            if not wanted or e.runtime_data.device_id in wanted or e.entry_id in wanted
        ]
        if not targets:
            raise ServiceValidationError(
                translation_domain=DOMAIN, translation_key="no_target", translation_placeholders={}
            )
        kind = data["cue"]
        param = data["count"] if kind == "crackle" else strength_param(data["strength"])
        for runtime in targets:
            if not runtime.entity(ROLE_CUE):
                raise ServiceValidationError(translation_domain=DOMAIN, translation_key="no_cue_entity")
            await runtime.async_send_cue(kind, param, priority=True)

    hass.services.async_register(DOMAIN, SERVICE_SEND_CUE, _send_cue, schema=SEND_CUE_SCHEMA)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: SoundConfigEntry) -> bool:
    conf = merged_config(entry)
    device_info, device_id = _build_device_info(hass, entry)
    keywords = {
        key: words
        for key in MOOD_KEYS
        if (words := parse_keywords(conf.get(f"{CONF_KEYWORDS_PREFIX}{key}"))) and key in MOODS_BY_KEY
    }
    runtime = SoundRuntime(
        hass=hass,
        entry_id=entry.entry_id,
        roles=roles_from_config(conf),
        device_info=device_info,
        device_id=device_id,
        media_player=conf.get(CONF_MEDIA_PLAYER) or None,
        keywords=keywords,
        sendspin_enabled=bool(conf.get(CONF_SENDSPIN)),
        sendspin_host=conf.get(CONF_SENDSPIN_HOST) or None,
        lead_ms=float(conf.get(CONF_LEAD_MS, DEFAULT_LEAD_MS)),
    )
    entry.runtime_data = runtime

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # Started after the platforms, so the switches have restored their state.
    if runtime.media_player:
        from .music import MusicFollower

        runtime.follower = MusicFollower(hass, runtime)
        entry.async_on_unload(runtime.follower.async_start())
    if runtime.sendspin_enabled:
        from .sendspin_bridge import SendspinBridge

        runtime.bridge = SendspinBridge(hass, entry, runtime)
        runtime.bridge.async_start()
        entry.async_on_unload(runtime.bridge.async_stop)

    entry.async_on_unload(entry.add_update_listener(_async_reload))
    return True


async def _async_reload(hass: HomeAssistant, entry: SoundConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: SoundConfigEntry) -> bool:
    runtime = entry.runtime_data
    if runtime.follower is not None:
        await runtime.follower.async_restore_now()
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
