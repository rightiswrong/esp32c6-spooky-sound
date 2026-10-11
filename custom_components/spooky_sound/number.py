"""Light lead time: how early to send a cue so the flash lands on the sound."""

from __future__ import annotations

from homeassistant.components.number import NumberMode, RestoreNumber
from homeassistant.const import EntityCategory, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import SoundConfigEntry, SoundRuntime
from .entity import SoundEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: SoundConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    runtime = entry.runtime_data
    if runtime.sendspin_enabled:
        async_add_entities([LeadTimeNumber(runtime)])


class LeadTimeNumber(SoundEntity, RestoreNumber):
    """Zigbee + Home Assistant delay to compensate (tune by eye: lower if the
    flash comes late, raise if it comes before the thunder)."""

    _attr_entity_category = EntityCategory.CONFIG
    _attr_icon = "mdi:timer-sand"
    _attr_mode = NumberMode.BOX
    _attr_native_min_value = 0
    _attr_native_max_value = 1000
    _attr_native_step = 10
    _attr_native_unit_of_measurement = UnitOfTime.MILLISECONDS

    def __init__(self, runtime: SoundRuntime) -> None:
        super().__init__(runtime, "lead_time")

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        if (last := await self.async_get_last_number_data()) is not None and last.native_value is not None:
            self.runtime.lead_ms = float(last.native_value)

    @property
    def native_value(self) -> float:
        return self.runtime.lead_ms

    async def async_set_native_value(self, value: float) -> None:
        self.runtime.lead_ms = float(value)
        self.async_write_ha_state()
