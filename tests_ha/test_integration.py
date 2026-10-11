"""Spooky Sound inside a real (test) Home Assistant.

The light's raw Zigbee entities are faked as plain states; number.set_value is
captured, which is exactly what the integration writes to the light.
"""

from __future__ import annotations

from datetime import timedelta

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.util import dt as dt_util
from pytest_homeassistant_custom_component.common import (
    MockConfigEntry,
    async_capture_events,
    async_fire_time_changed,
    async_mock_service,
)

from custom_components.spooky_sound.const import DOMAIN

IEEE = "00:12:4b:00:2a:3b:4c:5d"
ROLES = {
    "effect_number": "number.xiao_effect",
    "cue_number": "number.xiao_sound_cue",
    "source_number": "number.xiao_sound_source",
    "event_sensor": "sensor.xiao_sound_event",
    "level_sensor": "sensor.xiao_sound_level",
    "sensitivity_number": "number.xiao_mic_sensitivity",
}


def _set_light_states(hass: HomeAssistant, effect: str = "1.0") -> None:
    hass.states.async_set(ROLES["effect_number"], effect)
    hass.states.async_set(ROLES["cue_number"], "0.0")
    hass.states.async_set(ROLES["source_number"], "3.0")
    hass.states.async_set(ROLES["event_sensor"], "0.0")
    hass.states.async_set(ROLES["level_sensor"], "12.0")
    hass.states.async_set(ROLES["sensitivity_number"], "50.0")


async def _setup(hass: HomeAssistant, effect: str = "1.0", **extra) -> tuple[MockConfigEntry, list]:
    """Set up an entry; return it and the captured number.set_value calls.

    The mock goes in AFTER setup: forwarding the number platform loads Home
    Assistant's real number component, which registers number.set_value.
    The raw light entities here are plain states, so writes made during setup
    (there should be none worth checking) go nowhere.
    """
    _set_light_states(hass, effect)
    entry = MockConfigEntry(domain=DOMAIN, title="Porch", data={"device_id": None, **ROLES, **extra})
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry, async_mock_service(hass, "number", "set_value")


def _entity(hass: HomeAssistant, platform: str, key: str) -> str:
    reg = er.async_get(hass)
    for ent in reg.entities.values():
        if ent.platform == DOMAIN and ent.domain == platform and ent.unique_id.endswith(f"_{key}"):
            return ent.entity_id
    raise AssertionError(f"no {platform} entity {key}")


async def test_config_flow_maps_zha_entities(hass: HomeAssistant) -> None:
    """The flow finds the light's entities from their ZHA names / endpoints."""
    config_entry = MockConfigEntry(domain="zha")
    config_entry.add_to_hass(hass)
    device = dr.async_get(hass).async_get_or_create(
        config_entry_id=config_entry.entry_id,
        identifiers={("zha", IEEE)},
        manufacturer="SpookyLab",
        model="XIAO-C6-SpookySound",
        name="XIAO Spooky",
    )
    reg = er.async_get(hass)
    made = {}
    for domain, ep, cluster, name in [
        ("number", 12, 13, "Effect"),
        ("number", 13, 13, "Effect speed"),
        ("number", 16, 13, "Sound source (0 off,1 mic,2 remote,3 both)"),
        ("number", 17, 13, "Mic sensitivity"),
        ("number", 18, 13, "Sound cue"),
        ("sensor", 19, 12, "Sound level"),
        ("sensor", 20, 12, "Sound event"),
    ]:
        ent = reg.async_get_or_create(
            domain, "zha", f"{IEEE}-{ep}-{cluster}", device_id=device.id, original_name=name, config_entry=config_entry
        )
        made[name] = ent.entity_id

    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    assert result["type"] is FlowResultType.FORM
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"device_id": device.id})
    assert result["step_id"] == "entities"
    schema = result["data_schema"].schema
    suggested = {str(k): k.description["suggested_value"] for k in schema if k.description}
    assert suggested["effect_number"] == made["Effect"]
    assert suggested["cue_number"] == made["Sound cue"]
    assert suggested["source_number"] == made["Sound source (0 off,1 mic,2 remote,3 both)"]
    assert suggested["event_sensor"] == made["Sound event"]

    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["errors"] == {"base": "nothing_mapped"}
    result = await hass.config_entries.flow.async_configure(result["flow_id"], suggested)
    assert result["step_id"] == "music"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"sendspin": False, "lead_ms": 120})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == "XIAO Spooky"
    assert result["data"]["cue_number"] == made["Sound cue"]


async def test_sound_source_select(hass: HomeAssistant) -> None:
    _entry, calls = await _setup(hass)
    select = _entity(hass, "select", "sound_source")
    assert hass.states.get(select).state == "both"
    await hass.services.async_call("select", "select_option", {"entity_id": select, "option": "microphone"}, blocking=True)
    assert calls[-1].data == {"entity_id": ROLES["source_number"], "value": 1}


async def test_sound_heard_event(hass: HomeAssistant) -> None:
    await _setup(hass)
    events = async_capture_events(hass, "spooky_sound_detected")
    event_entity = _entity(hass, "event", "sound_event")
    hass.states.async_set(ROLES["event_sensor"], "11.0")      # seq 1, thunder
    await hass.async_block_till_done()
    state = hass.states.get(event_entity)
    assert state.attributes["event_type"] == "thunder"
    hass.states.async_set(ROLES["event_sensor"], "23.0")      # seq 2, scream
    await hass.async_block_till_done()
    assert hass.states.get(event_entity).attributes["event_type"] == "loud"
    assert [e.data["type"] for e in events] == ["thunder", "loud"]
    # unavailable → same value again is not a new sound
    hass.states.async_set(ROLES["event_sensor"], "unavailable")
    await hass.async_block_till_done()
    hass.states.async_set(ROLES["event_sensor"], "23.0")
    await hass.async_block_till_done()
    assert len(events) == 2


async def test_test_thunder_button_and_send_cue(hass: HomeAssistant) -> None:
    _entry, calls = await _setup(hass)
    await hass.services.async_call(
        "button", "press", {"entity_id": _entity(hass, "button", "test_thunder")}, blocking=True
    )
    await hass.async_block_till_done()
    assert calls[-1].data == {"entity_id": ROLES["cue_number"], "value": 1 * 10000 + 1 * 1000 + 899}
    await hass.services.async_call(DOMAIN, "send_cue", {"cue": "crackle", "count": 5}, blocking=True)
    await hass.async_block_till_done()
    assert calls[-1].data["value"] == 2 * 10000 + 4 * 1000 + 5


async def test_follows_the_soundtrack(hass: HomeAssistant) -> None:
    hass.states.async_set("media_player.echo_show", "idle")
    _entry, calls = await _setup(hass, media_player="media_player.echo_show")
    mood = _entity(hass, "sensor", "soundtrack_mood")
    assert hass.states.get(mood).state == "none"

    hass.states.async_set("media_player.echo_show", "playing", {"media_title": "Rain & Thunderstorm, 10 hours"})
    await hass.async_block_till_done()
    assert hass.states.get(mood).state == "storm"
    effect_writes = [c.data["value"] for c in calls if c.data["entity_id"] == ROLES["effect_number"]]
    assert effect_writes == [3]                                # Lightning Storm

    hass.states.async_set("media_player.echo_show", "playing", {"media_title": "Crackling Fireplace"})
    await hass.async_block_till_done()
    assert hass.states.get(mood).state == "fire"
    effect_writes = [c.data["value"] for c in calls if c.data["entity_id"] == ROLES["effect_number"]]
    assert effect_writes == [3, 1]                             # Candle Flicker

    hass.states.async_set("media_player.echo_show", "paused", {"media_title": "Crackling Fireplace"})
    await hass.async_block_till_done()
    assert hass.states.get(mood).state == "none"
    async_fire_time_changed(hass, dt_util.utcnow() + timedelta(seconds=16))
    await hass.async_block_till_done()
    effect_writes = [c.data["value"] for c in calls if c.data["entity_id"] == ROLES["effect_number"]]
    assert effect_writes[-1] == 1                              # back to what it was before the storm


async def test_follow_switch_off_restores(hass: HomeAssistant) -> None:
    hass.states.async_set("media_player.echo_show", "playing", {"media_title": "Heartbeat sound effect"})
    entry, calls = await _setup(hass, effect="9.0", media_player="media_player.echo_show")
    assert entry.runtime_data.mood == "heartbeat"      # (it switched to Tell-Tale Heart during setup)
    switch = _entity(hass, "switch", "follow_music")
    assert hass.states.get(switch).state == "on"
    await hass.services.async_call("switch", "turn_off", {"entity_id": switch}, blocking=True)
    await hass.async_block_till_done()
    effect_writes = [c.data["value"] for c in calls if c.data["entity_id"] == ROLES["effect_number"]]
    assert effect_writes == [9]                         # Blood Moon is back
    await hass.services.async_call("switch", "turn_on", {"entity_id": switch}, blocking=True)
    await hass.async_block_till_done()
    effect_writes = [c.data["value"] for c in calls if c.data["entity_id"] == ROLES["effect_number"]]
    assert effect_writes == [9, 5]                      # and the heart again


async def test_sendspin_without_server(hass: HomeAssistant) -> None:
    """No Music Assistant to talk to: the bridge says so instead of failing setup."""
    await _setup(hass, sendspin=True)
    status = _entity(hass, "sensor", "sendspin_status")
    assert hass.states.get(status).state == "no_server"
    lead = _entity(hass, "number", "lead_time")
    assert float(hass.states.get(lead).state) == 120
    await hass.services.async_call("number", "set_value", {"entity_id": lead, "value": 200}, blocking=True)
    assert float(hass.states.get(lead).state) == 200


async def test_unload(hass: HomeAssistant) -> None:
    entry, _calls = await _setup(hass, media_player="media_player.echo_show", sendspin=True)
    assert await hass.config_entries.async_unload(entry.entry_id)
    await hass.async_block_till_done()


async def test_options_flow(hass: HomeAssistant) -> None:
    entry, _calls = await _setup(hass)
    result = await hass.config_entries.options.async_init(entry.entry_id)
    assert result["step_id"] == "init"
    result = await hass.config_entries.options.async_configure(result["flow_id"], ROLES)
    assert result["step_id"] == "music"
    result = await hass.config_entries.options.async_configure(
        result["flow_id"], {"media_player": "media_player.echo_show", "keywords_fire": "yule log", "sendspin": False}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    await hass.async_block_till_done()
    assert entry.runtime_data.media_player == "media_player.echo_show"
    assert entry.runtime_data.keywords["fire"] == ("yule log",)


@pytest.mark.parametrize("value", ["1.0", "unavailable"])
async def test_survives_odd_states(hass: HomeAssistant, value: str) -> None:
    await _setup(hass)
    hass.states.async_set(ROLES["source_number"], value)
    await hass.async_block_till_done()
    assert hass.states.get(_entity(hass, "select", "sound_source")).state in ("microphone", "unavailable", "unknown")
