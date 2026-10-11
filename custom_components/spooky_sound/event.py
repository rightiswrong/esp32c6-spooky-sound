"""The "Sound heard" event: thunder, a crackling fire or a scream, as the light's microphone hears it."""

from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.core import Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SoundConfigEntry, SoundRuntime
from .const import EVENT_SOUND_DETECTED, EVENT_TYPES, ROLE_EVENT
from .entity import SoundEntity
from .logic import decode_event


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    if runtime.entity(ROLE_EVENT):
        async_add_entities([SoundHeardEvent(runtime)])


class SoundHeardEvent(SoundEntity, EventEntity):
    _source_roles = (ROLE_EVENT,)
    _primary_role = ROLE_EVENT
    _attr_icon = "mdi:waveform"
    _attr_event_types = list(EVENT_TYPES.values())

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "sound_event")

    @callback
    def _source_changed(self, event: Event[EventStateChangedData]) -> None:
        old, new = event.data["old_state"], event.data["new_state"]
        if new is None:
            return
        decoded = decode_event(new.state)
        previous = decode_event(old.state) if old is not None else None
        # A new sequence number = a new sound. (Restarts and "unavailable"
        # blips report the same value again; those are not new sounds.)
        if decoded is None or decoded == previous or old is None or old.state in ("unknown", "unavailable"):
            self.async_write_ha_state()
            return
        _seq, kind = decoded
        self._trigger_event(kind)
        self.async_write_ha_state()
        self.hass.bus.async_fire(
            EVENT_SOUND_DETECTED,
            {"device_id": self.runtime.device_id, "entry_id": self.runtime.entry_id, "type": kind},
        )
