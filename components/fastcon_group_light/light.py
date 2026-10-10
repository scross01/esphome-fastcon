import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_COLOR_INTERLOCK, CONF_OUTPUT_ID
from esphome.core import HexInt

DEPENDENCIES = ["fastcon", "light"]
AUTO_LOAD = ["light"]

CONF_CONTROLLER_ID = "controller_id"
CONF_MESH_KEY = "mesh_key"
CONF_START_LIGHT_ID = "start_light_id"
CONF_MASK = "mask"
CONF_DEVICE_TYPE = "device_type"

# brMesh device types (see FirstFragment.java:629-676)
DEVICE_TYPES = {
    "pwr": 43049,
    "rgbcw": 43050,
    "cct": 43051,
    "rgb": 43168,
    "rgbw": 43169,
}

fastcon_ns = cg.esphome_ns.namespace("fastcon")
FastconController = fastcon_ns.class_("FastconController", cg.Component)

group_ns = cg.esphome_ns.namespace("fastcon_group_light")
FastconGroupLight = group_ns.class_(
    "FastconGroupLight", light.LightOutput, cg.Component
)


def validate_hex_bytes(value):
    if not isinstance(value, str):
        raise cv.Invalid("Mesh key must be a string")
    value = value.replace(" ", "")
    if len(value) != 8:
        raise cv.Invalid("Mesh key must be exactly 8 hex characters")
    try:
        return HexInt(int(value, 16))
    except ValueError as err:
        raise cv.Invalid(f"Invalid hex value: {err}")


CONFIG_SCHEMA = (
    light.RGB_LIGHT_SCHEMA
    .extend({
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(FastconGroupLight),
            cv.Optional(CONF_CONTROLLER_ID, default="fastcon_controller"): cv.use_id(FastconController),
            cv.Optional(CONF_MESH_KEY): validate_hex_bytes,
        cv.Required(CONF_START_LIGHT_ID): cv.int_range(min=1, max=255),
        cv.Required(CONF_MASK): cv.int_range(min=1, max=255),
        cv.Optional(CONF_DEVICE_TYPE, default="rgbcw"): cv.enum(DEVICE_TYPES, lower=True),
        cv.Optional(CONF_COLOR_INTERLOCK, default=False): cv.boolean,
    })
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])

    await cg.register_component(var, config)
    await light.register_light(var, config)

    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(var.set_controller(controller))

    if CONF_MESH_KEY in config:
        mesh_key = config[CONF_MESH_KEY]
        key_bytes = [(mesh_key >> (i * 8)) & 0xFF for i in range(3, -1, -1)]
        cg.add(var.set_mesh_key(key_bytes))

    cg.add(var.set_start_light_id(config[CONF_START_LIGHT_ID]))
    cg.add(var.set_mask(config[CONF_MASK]))

    device_type = DEVICE_TYPES[config[CONF_DEVICE_TYPE]]
    cg.add(var.set_device_type(device_type))

    if config.get(CONF_COLOR_INTERLOCK):
        cg.add(var.set_color_interlock(True))
