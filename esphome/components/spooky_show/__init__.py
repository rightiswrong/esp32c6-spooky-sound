"""spooky_show — Spooky Lights' Halloween effects, with ears, for ESPHome PWM lights.

    spooky_show:
      microphone_pin: GPIO33          # optional; an ADC1 pin
      sound_level:   {name: Sound level}
      microphone:    {name: Microphone}
      sound_heard:   {name: Sound heard}

    light:
      - platform: rgbww
        ...
        effects:
          - spooky: {effect: lightning}       # name defaults to "Lightning Storm"
          - spooky: {effect: candle}
"""

import esphome.codegen as cg
from esphome.components import binary_sensor, event, sensor
from esphome.components.light.effects import register_rgb_effect
from esphome.components.light.types import LightEffect
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

CODEOWNERS = ["@rightiswrong"]
DEPENDENCIES = ["esp32", "light"]
AUTO_LOAD = ["sensor", "binary_sensor", "event"]

CONF_MICROPHONE_PIN = "microphone_pin"
CONF_SAMPLE_RATE = "sample_rate"
CONF_SOUND_LEVEL = "sound_level"
CONF_MICROPHONE = "microphone"
CONF_SOUND_HEARD = "sound_heard"
CONF_SPOOKY_SHOW_ID = "spooky_show_id"
CONF_EFFECT = "effect"

spooky_ns = cg.esphome_ns.namespace("spooky_show")
SpookyShow = spooky_ns.class_("SpookyShow", cg.Component)
SpookyLightEffect = spooky_ns.class_("SpookyLightEffect", LightEffect)

# key → (firmware FX_* index, display name). Names are shared with the XIAO /
# NanoC6 firmware and the Spooky Sound integration (tests/test_logic.py checks).
EFFECTS = {
    "solid": (0, "Solid"),
    "candle": (1, "Candle Flicker"),
    "ghost": (2, "Ghost Breath"),
    "lightning": (3, "Lightning Storm"),
    "cauldron": (4, "Witch's Cauldron"),
    "heartbeat": (5, "Tell-Tale Heart"),
    "chase": (6, "Haunted Chase"),
    "poltergeist": (7, "Poltergeist"),
    "pumpkin": (8, "Possessed Pumpkin"),
    "blood_moon": (9, "Blood Moon"),
}
EVENT_TYPES = ["thunder", "fire", "loud"]


def _microphone_pin(value):
    """An ADC1-capable GPIO number (the classic ESP32's are 32–39)."""
    if isinstance(value, str) and value.upper().startswith("GPIO"):
        value = value[4:]
    return cv.int_range(min=0, max=48)(cv.int_(value))


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SpookyShow),
        cv.Optional(CONF_MICROPHONE_PIN): _microphone_pin,
        cv.Optional(CONF_SAMPLE_RATE, default=32000): cv.int_range(min=20000, max=48000),
        cv.Optional(CONF_SOUND_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon="mdi:waveform",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MICROPHONE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            icon="mdi:microphone",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_SOUND_HEARD): event.event_schema(icon="mdi:ghost"),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_MICROPHONE_PIN in config:
        cg.add(var.set_microphone_pin(config[CONF_MICROPHONE_PIN]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    if conf := config.get(CONF_SOUND_LEVEL):
        cg.add(var.set_level_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_MICROPHONE):
        cg.add(var.set_microphone_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_SOUND_HEARD):
        cg.add(var.set_sound_event(await event.new_event(conf, event_types=EVENT_TYPES)))


def _default_effect_name(config):
    if config[CONF_NAME] == "Spooky":
        config = dict(config)
        config[CONF_NAME] = EFFECTS[config[CONF_EFFECT]][1]
    return config


@register_rgb_effect(
    "spooky",
    SpookyLightEffect,
    "Spooky",
    {
        cv.GenerateID(CONF_SPOOKY_SHOW_ID): cv.use_id(SpookyShow),
        cv.Required(CONF_EFFECT): cv.one_of(*EFFECTS, lower=True),
    },
    _default_effect_name,
)
async def spooky_effect_to_code(config, effect_id):
    show = await cg.get_variable(config[CONF_SPOOKY_SHOW_ID])
    return cg.new_Pvariable(effect_id, config[CONF_NAME], show, EFFECTS[config[CONF_EFFECT]][0])
