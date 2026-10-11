"""Config flow: pick the light, confirm its entities, then the music side."""

from __future__ import annotations

from typing import Any

import voluptuous as vol
from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import selector

from .const import (
    CONF_DEVICE_ID,
    CONF_KEYWORDS_PREFIX,
    CONF_LEAD_MS,
    CONF_MEDIA_PLAYER,
    CONF_SENDSPIN,
    CONF_SENDSPIN_HOST,
    DEFAULT_LEAD_MS,
    DOMAIN,
    MOODS,
    ROLE_CUE,
    ROLE_DOMAINS,
    ROLE_EFFECT,
    ROLE_EVENT,
    ROLE_LEVEL,
    ROLE_LIGHT,
    ROLE_SENSITIVITY,
    ROLE_SOURCE,
)
from .logic import guess_role

ROLE_ORDER = (ROLE_EFFECT, ROLE_CUE, ROLE_SOURCE, ROLE_EVENT, ROLE_LEVEL, ROLE_SENSITIVITY, ROLE_LIGHT)
USEFUL_ROLES = (ROLE_EFFECT, ROLE_CUE, ROLE_SOURCE, ROLE_EVENT)
MUSIC_KEYS = (CONF_MEDIA_PLAYER, CONF_SENDSPIN, CONF_SENDSPIN_HOST, CONF_LEAD_MS)


def _entities_schema() -> vol.Schema:
    return vol.Schema(
        {
            vol.Optional(role): selector.EntitySelector(selector.EntitySelectorConfig(domain=ROLE_DOMAINS[role]))
            for role in ROLE_ORDER
        }
    )


def _music_schema(with_keywords: bool) -> vol.Schema:
    fields: dict[Any, Any] = {
        vol.Optional(CONF_MEDIA_PLAYER): selector.EntitySelector(selector.EntitySelectorConfig(domain="media_player")),
        vol.Optional(CONF_SENDSPIN, default=False): selector.BooleanSelector(),
        vol.Optional(CONF_SENDSPIN_HOST): selector.TextSelector(),
        vol.Optional(CONF_LEAD_MS, default=DEFAULT_LEAD_MS): selector.NumberSelector(
            selector.NumberSelectorConfig(min=0, max=1000, step=10, unit_of_measurement="ms", mode="box")
        ),
    }
    if with_keywords:
        for mood in MOODS:
            fields[vol.Optional(f"{CONF_KEYWORDS_PREFIX}{mood.key}")] = selector.TextSelector()
    return vol.Schema(fields)


SPOOKY_MANUFACTURER = "SpookyLab"


def _device_selector() -> selector.DeviceSelector:
    """SpookyLab lights paired with ZHA (preferred) or Zigbee2MQTT."""
    return selector.DeviceSelector(
        selector.DeviceSelectorConfig(
            filter=[
                selector.DeviceFilterSelectorConfig(integration="zha", manufacturer=SPOOKY_MANUFACTURER),
                selector.DeviceFilterSelectorConfig(integration="mqtt", manufacturer=SPOOKY_MANUFACTURER),
            ]
        )
    )


def unconfigured_zha_lights(hass: HomeAssistant, configured: set[str]) -> list[str]:
    """Device ids of SpookyLab lights in ZHA that don't have Spooky Sound yet."""
    found = []
    for device in dr.async_get(hass).devices.values():
        if device.manufacturer != SPOOKY_MANUFACTURER or device.id in configured:
            continue
        if any(domain == "zha" for domain, _ in device.identifiers):
            found.append(device.id)
    return found


def guess_entities(hass: HomeAssistant, device_id: str) -> dict[str, str]:
    """Map the device's entities to roles (first match wins)."""
    found: dict[str, str] = {}
    for entry in er.async_entries_for_device(er.async_get(hass), device_id, include_disabled_entities=False):
        role = guess_role(entry.domain, entry.unique_id, entry.original_name, entry.name, entry.entity_id)
        if role and role not in found:
            found[role] = entry.entity_id
    return found


def _clean(user_input: dict[str, Any], keys) -> dict[str, Any]:
    """Blank optional fields must clear a previous value."""
    return {key: user_input.get(key) or "" for key in keys} | {
        k: v for k, v in user_input.items() if k not in keys
    }


class SpookySoundConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._device_id: str | None = None
        self._title = "Spooky Sound"
        self._data: dict[str, Any] = {}

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            self._device_id = user_input.get(CONF_DEVICE_ID)
            if self._device_id:
                await self.async_set_unique_id(self._device_id)
                self._abort_if_unique_id_configured()
                if device := dr.async_get(self.hass).async_get(self._device_id):
                    self._title = device.name_by_user or device.name or self._title
            return await self.async_step_entities()
        # ZHA first: if exactly one SpookyLab light in ZHA is waiting, pick it.
        configured = {e.unique_id for e in self._async_current_entries() if e.unique_id}
        waiting = unconfigured_zha_lights(self.hass, configured)
        schema = vol.Schema({vol.Optional(CONF_DEVICE_ID): _device_selector()})
        if len(waiting) == 1:
            schema = self.add_suggested_values_to_schema(schema, {CONF_DEVICE_ID: waiting[0]})
        return self.async_show_form(
            step_id="user", data_schema=schema, description_placeholders={"found": str(len(waiting))}
        )

    async def async_step_entities(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            if not any(user_input.get(role) for role in USEFUL_ROLES):
                errors["base"] = "nothing_mapped"
            else:
                if not self._device_id:
                    first = next(user_input[r] for r in USEFUL_ROLES if user_input.get(r))
                    await self.async_set_unique_id(first)
                    self._abort_if_unique_id_configured()
                self._data = {CONF_DEVICE_ID: self._device_id, **user_input}
                return await self.async_step_music()
        suggested = (
            user_input
            if user_input is not None
            else (guess_entities(self.hass, self._device_id) if self._device_id else {})
        )
        return self.async_show_form(
            step_id="entities",
            data_schema=self.add_suggested_values_to_schema(_entities_schema(), suggested),
            errors=errors,
            description_placeholders={"found": str(len(suggested))},
        )

    async def async_step_music(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(title=self._title, data={**self._data, **user_input})
        return self.async_show_form(step_id="music", data_schema=_music_schema(with_keywords=False))

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return SpookySoundOptionsFlow()


class SpookySoundOptionsFlow(OptionsFlow):
    """Re-map entities, or change the music side, at any time."""

    def __init__(self) -> None:
        self._options: dict[str, Any] = {}

    def _current(self) -> dict[str, Any]:
        return {**self.config_entry.data, **self.config_entry.options}

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            if not any(user_input.get(role) for role in USEFUL_ROLES):
                errors["base"] = "nothing_mapped"
            else:
                self._options = _clean(user_input, ROLE_ORDER)
                return await self.async_step_music()
        current = {k: v for k, v in self._current().items() if k in ROLE_DOMAINS and v}
        return self.async_show_form(
            step_id="init",
            data_schema=self.add_suggested_values_to_schema(_entities_schema(), user_input or current),
            errors=errors,
        )

    async def async_step_music(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        keyword_keys = tuple(f"{CONF_KEYWORDS_PREFIX}{m.key}" for m in MOODS)
        if user_input is not None:
            cleaned = _clean(user_input, (CONF_MEDIA_PLAYER, CONF_SENDSPIN_HOST, *keyword_keys))
            return self.async_create_entry(data={**self._options, **cleaned})
        current = self._current()
        suggested = {k: current[k] for k in (*MUSIC_KEYS, *keyword_keys) if current.get(k) not in (None, "")}
        for mood in MOODS:
            suggested.setdefault(f"{CONF_KEYWORDS_PREFIX}{mood.key}", ", ".join(mood.keywords))
        return self.async_show_form(
            step_id="music",
            data_schema=self.add_suggested_values_to_schema(_music_schema(with_keywords=True), suggested),
        )
