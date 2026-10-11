"""Test thunder: flash the light the way a soundtrack's thunderclap would."""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SoundConfigEntry, SoundRuntime
from .const import ROLE_CUE
from .entity import SoundEntity
from .logic import strength_param


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    if runtime.entity(ROLE_CUE):
        async_add_entities([TestThunderButton(runtime)])


class TestThunderButton(SoundEntity, ButtonEntity):
    _primary_role = ROLE_CUE
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_icon = "mdi:weather-lightning"

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "test_thunder")

    async def async_press(self) -> None:
        await self.runtime.async_send_cue("thunder", strength_param(0.9), priority=True)
