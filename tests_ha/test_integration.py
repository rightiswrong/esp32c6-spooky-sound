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


async def _setup(
    hass: HomeAssistant, effect: str = "1.0", mock_numbers: bool = True, **extra
) -> tuple[MockConfigEntry, list]:
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
    return entry, (async_mock_service(hass, "number", "set_value") if mock_numbers else [])


def _entity(hass: HomeAssistant, platform: str, key: str) -> str:
    reg = er.async_get(hass)
    for ent in reg.entities.values():
        if ent.platform == DOMAIN and ent.domain == platform and ent.unique_id.endswith(f"_{key}"):
            return ent.entity_id
    raise AssertionError(f"no {platform} entity {key}")


# What ZHA really creates for the light: numbers "<ieee>-<ep>-13" (Analog
# Output), sensors "<ieee>-<ep>-12-analog_input" (Analog Input), named from
# the firmware's ZCL description attribute.
ZHA_ENTITIES = [
    ("number", "12-13", "Effect"),
    ("number", "13-13", "Effect speed"),
    ("number", "15-13", "Spotlight pixel (0 = off)"),
    ("number", "16-13", "Sound source (0 off,1 mic,2 remote,3 both)"),
    ("number", "17-13", "Mic sensitivity"),
    ("number", "18-13", "Sound cue"),
    ("sensor", "11-12-analog_input", "Grove LEDs detected"),
    ("sensor", "19-12-analog_input", "Sound level"),
    ("sensor", "20-12-analog_input", "Sound event"),
]


def _zha_entities(hass: HomeAssistant, config_entry: MockConfigEntry, device_id: str) -> dict[str, str]:
    reg = er.async_get(hass)
    made = {}
    for domain, suffix, name in ZHA_ENTITIES:
        ent = reg.async_get_or_create(
            domain, "zha", f"{IEEE}-{suffix}", device_id=device_id, original_name=name, config_entry=config_entry
        )
        made[name] = ent.entity_id
    return made


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
    made = _zha_entities(hass, config_entry, device.id)

    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    assert result["type"] is FlowResultType.FORM
    # ZHA first: the only SpookyLab light in ZHA is pre-selected.
    picker = {str(k): k.description for k in result["data_schema"].schema}
    assert picker["device_id"]["suggested_value"] == device.id
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


async def test_raw_zha_entities_hidden_then_restored(hass: HomeAssistant) -> None:
    """The raw ZHA cue/source/event entities hide behind ours, and come back on removal."""
    zha_entry = MockConfigEntry(domain="zha")
    zha_entry.add_to_hass(hass)
    device = dr.async_get(hass).async_get_or_create(
        config_entry_id=zha_entry.entry_id, identifiers={("zha", IEEE)}, manufacturer="SpookyLab", name="XIAO"
    )
    made = _zha_entities(hass, zha_entry, device.id)
    reg = er.async_get(hass)
    reg.async_update_entity(made["Mic sensitivity"], hidden_by=er.RegistryEntryHider.USER)   # the user's own choice
    roles = {
        "effect_number": made["Effect"],
        "cue_number": made["Sound cue"],
        "source_number": made["Sound source (0 off,1 mic,2 remote,3 both)"],
        "event_sensor": made["Sound event"],
        "level_sensor": made["Sound level"],
        "sensitivity_number": made["Mic sensitivity"],
    }
    for entity_id in roles.values():
        hass.states.async_set(entity_id, "0.0")
    entry = MockConfigEntry(domain=DOMAIN, title="XIAO", unique_id=device.id, data={"device_id": device.id, **roles})
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()

    hidden = {name for name, eid in made.items() if reg.async_get(eid).hidden_by is not None}
    assert hidden == {"Sound cue", "Sound source (0 off,1 mic,2 remote,3 both)", "Sound event", "Mic sensitivity"}
    assert reg.async_get(made["Sound cue"]).hidden_by is er.RegistryEntryHider.INTEGRATION
    # Our entities live on the same ZHA device.
    ours = [e for e in reg.entities.values() if e.platform == DOMAIN]
    assert ours and all(e.device_id == device.id for e in ours)

    assert await hass.config_entries.async_remove(entry.entry_id)
    await hass.async_block_till_done()
    assert reg.async_get(made["Sound cue"]).hidden_by is None
    assert reg.async_get(made["Sound event"]).hidden_by is None
    assert reg.async_get(made["Mic sensitivity"]).hidden_by is er.RegistryEntryHider.USER   # untouched


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
    await _setup(hass, mock_numbers=False, sendspin=True)
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


# ------------------------------------------------------------- ESPHome strip
EFFECT_LIST = [
    "None", "Solid", "Candle Flicker", "Ghost Breath", "Lightning Storm", "Witch's Cauldron",
    "Tell-Tale Heart", "Haunted Chase", "Poltergeist", "Possessed Pumpkin", "Blood Moon",
]
MAC = "a0b1c2d3e4f5"
# What ESPHome creates for the Gledopto strip firmware (esphome/packages/base.yaml).
ESPHOME_ENTITIES = [
    ("light", "light-spooky_overhead", None),
    ("number", "number-effect_speed", "Effect speed"),
    ("number", "number-mic_sensitivity", "Mic sensitivity"),
    ("number", "number-sound_cue", "Sound cue"),
    ("select", "select-sound_source", "Sound source"),
    ("sensor", "sensor-sound_level", "Sound level"),
    ("event", "event-sound_heard", "Sound heard"),
]


async def test_config_flow_maps_esphome_strip(hass: HomeAssistant) -> None:
    """The overhead strip: its light carries the effects, its number takes cues."""
    config_entry = MockConfigEntry(domain="esphome")
    config_entry.add_to_hass(hass)
    device = dr.async_get(hass).async_get_or_create(
        config_entry_id=config_entry.entry_id,
        connections={(dr.CONNECTION_NETWORK_MAC, MAC)},
        manufacturer="SpookyLab",
        model="spooky-overhead",
        name="Spooky Overhead",
    )
    reg = er.async_get(hass)
    made = {}
    for domain, suffix, name in ESPHOME_ENTITIES:
        ent = reg.async_get_or_create(
            domain, "esphome", f"{MAC}-{suffix}", device_id=device.id, original_name=name, config_entry=config_entry
        )
        made[suffix] = ent.entity_id
    hass.states.async_set(made["light-spooky_overhead"], "on", {"effect_list": EFFECT_LIST, "effect": "None"})

    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"device_id": device.id})
    assert result["step_id"] == "entities"
    suggested = {
        str(k): k.description["suggested_value"] for k in result["data_schema"].schema if k.description
    }
    assert suggested["effect_number"] == made["light-spooky_overhead"]
    assert suggested["cue_number"] == made["number-sound_cue"]
    assert not suggested.get("sensitivity_number")      # native entities stay native
    assert not suggested.get("level_sensor")


async def test_follows_the_soundtrack_on_a_light(hass: HomeAssistant) -> None:
    """Effect role = an ESPHome light: switch its effect by name, restore 'None'."""
    light = "light.spooky_overhead"
    hass.states.async_set(light, "on", {"effect_list": EFFECT_LIST, "effect": "None"})
    hass.states.async_set("number.spooky_overhead_sound_cue", "0")
    hass.states.async_set("media_player.echo_show", "idle")
    entry = MockConfigEntry(
        domain=DOMAIN,
        title="Overhead",
        data={
            "device_id": None,
            "effect_number": light,
            "cue_number": "number.spooky_overhead_sound_cue",
            "media_player": "media_player.echo_show",
        },
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    calls = async_mock_service(hass, "light", "turn_on")

    hass.states.async_set("media_player.echo_show", "playing", {"media_title": "Rain & Thunderstorm, 10 hours"})
    await hass.async_block_till_done()
    assert [c.data for c in calls] == [{"entity_id": light, "effect": "Lightning Storm"}]

    hass.states.async_set(light, "on", {"effect_list": EFFECT_LIST, "effect": "Lightning Storm"})
    hass.states.async_set("media_player.echo_show", "idle")
    await hass.async_block_till_done()
    async_fire_time_changed(hass, dt_util.utcnow() + timedelta(seconds=16))
    await hass.async_block_till_done()
    assert calls[-1].data == {"entity_id": light, "effect": "None"}

    # A light someone switched off is never switched on by the soundtrack.
    hass.states.async_set(light, "off", {"effect_list": EFFECT_LIST})
    hass.states.async_set("media_player.echo_show", "playing", {"media_title": "Crackling Fireplace"})
    await hass.async_block_till_done()
    assert len(calls) == 2
