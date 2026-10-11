"""Switches: follow the soundtrack's mood, and sync the light to the music (Sendspin)."""

from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import STATE_OFF
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.restore_state import RestoreEntity

from . import SoundConfigEntry, SoundRuntime
from .entity import SoundEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    entities: list[SwitchEntity] = []
    if runtime.media_player:
        entities.append(FollowMusicSwitch(runtime))
    if runtime.sendspin_enabled:
        entities.append(MusicSyncSwitch(runtime))
    async_add_entities(entities)


class _RuntimeFlagSwitch(SoundEntity, SwitchEntity, RestoreEntity):
    """A switch backed by a boolean on the runtime, remembered across restarts."""

    _flag: str

    @property
    def is_on(self) -> bool:
        return bool(getattr(self.runtime, self._flag))

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        if (last := await self.async_get_last_state()) is not None:
            setattr(self.runtime, self._flag, last.state != STATE_OFF)

    async def _set(self, value: bool) -> None:
        setattr(self.runtime, self._flag, value)
        self.async_write_ha_state()

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._set(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._set(False)


class FollowMusicSwitch(_RuntimeFlagSwitch):
    """On: a thunderstorm track switches the light to Lightning Storm, and so on."""

    _flag = "follow_music"
    _attr_icon = "mdi:music-note"

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "follow_music")

    async def _set(self, value: bool) -> None:
        await super()._set(value)
        if (follower := self.runtime.follower) is not None:
            if value:
                await follower.async_apply()
            else:
                await follower.async_restore_now()


class MusicSyncSwitch(_RuntimeFlagSwitch):
    """On: Sendspin visualizer data drives timed cues to the light."""

    _flag = "music_sync"
    _attr_icon = "mdi:metronome"

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "music_sync")
