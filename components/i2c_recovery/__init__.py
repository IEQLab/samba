import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = ["i2c"]

CONF_MIN_INTERVAL = "min_interval"

i2c_recovery_ns = cg.esphome_ns.namespace("i2c_recovery")
I2CRecovery = i2c_recovery_ns.class_("I2CRecovery", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(I2CRecovery),
        cv.Optional(CONF_PORT, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_MIN_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_min_interval(config[CONF_MIN_INTERVAL]))
