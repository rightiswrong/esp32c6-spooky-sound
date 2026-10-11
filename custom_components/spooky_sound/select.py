"""Sound source picker: off · microphone · remote (HA / Music Assistant) · both."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SoundConfigEntry, SoundRuntime
from .const import ROLE_SOURCE, SOUND_SOURCES
from .entity import SoundEntity
from .logic import to_index


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    if runtime.entity(ROLE_SOURCE):
        async_add_entities([SoundSourceSelect(runtime)])


class SoundSourceSelect(SoundEntity, SelectEntity):
    _source_roles = (ROLE_SOURCE,)
    _primary_role = ROLE_SOURCE
    _attr_icon = "mdi:ear-hearing"
    _attr_options = list(SOUND_SOURCES)

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "sound_source")

    @property
    def current_option(self) -> str | None:
        src = self.runtime.state(ROLE_SOURCE)
        if src is None or src.state in ("unknown", "unavailable"):
            return None
        return SOUND_SOURCES[to_index(src.state, len(SOUND_SOURCES) - 1)]

    async def async_select_option(self, option: str) -> None:
        await self.runtime.async_set_number(ROLE_SOURCE, SOUND_SOURCES.index(option))
