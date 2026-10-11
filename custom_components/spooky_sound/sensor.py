"""Soundtrack mood (from the followed media player) and Sendspin bridge status."""

from __future__ import annotations

from typing import Any

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SoundConfigEntry, SoundRuntime
from .const import MOOD_KEYS, MOODS
from .entity import SoundEntity

BRIDGE_STATES = ["off", "installing", "connecting", "waiting", "streaming", "no_server", "error"]


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    entities: list[SensorEntity] = []
    if runtime.media_player:
        entities.append(SoundtrackMoodSensor(runtime))
    if runtime.sendspin_enabled:
        entities.append(SendspinStatusSensor(runtime))
    async_add_entities(entities)


class SoundtrackMoodSensor(SoundEntity, SensorEntity):
    _follows_runtime = True
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = ["none", *MOOD_KEYS]
    _attr_icon = "mdi:music-clef-treble"

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "soundtrack_mood")

    @property
    def native_value(self) -> str:
        return self.runtime.mood or "none"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        effect = next((m.effect for m in MOODS if m.key == self.runtime.mood), None)
        return {"media_player": self.runtime.media_player, "effect": effect}


class SendspinStatusSensor(SoundEntity, SensorEntity):
    _follows_runtime = True
    _attr_device_class = SensorDeviceClass.ENUM
    _attr_options = BRIDGE_STATES
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_icon = "mdi:access-point-network"

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "sendspin_status")

    @property
    def native_value(self) -> str:
        return self.runtime.bridge_status if self.runtime.bridge_status in BRIDGE_STATES else "error"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        bridge = self.runtime.bridge
        return {
            "server": getattr(bridge, "url", None),
            "error": getattr(bridge, "error", None),
            "frames_received": getattr(bridge, "frames", 0),
            "cues_sent": self.runtime.cues_sent,
            "cues_dropped": self.runtime.cues_dropped,
            "focus": self.runtime.focus(),
            "lead_ms": self.runtime.lead_ms,
        }
