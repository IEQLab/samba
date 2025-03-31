from esphome import core
import esphome.codegen as cg
from esphome.components import i2c, sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_CO2,
    CONF_ID,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_CARBON_DIOXIDE,
    ICON_MOLECULE_CO2,
    STATE_CLASS_MEASUREMENT,
    UNIT_PARTS_PER_MILLION,
)

DEPENDENCIES = ["i2c"]

k30_ns = cg.esphome_ns.namespace("k30")
K30Component = k30_ns.class_("K30Component", cg.PollingComponent, i2c.I2CDevice)

CONF_AUTOMATIC_SELF_CALIBRATION = "use_abc"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(K30Component),
            cv.Optional(CONF_CO2): sensor.sensor_schema(
                unit_of_measurement=UNIT_PARTS_PER_MILLION,
                icon=ICON_MOLECULE_CO2,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_CARBON_DIOXIDE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_AUTOMATIC_SELF_CALIBRATION, default=True): cv.boolean,
            cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(
                    min=core.TimePeriod(seconds=1), max=core.TimePeriod(seconds=1800)
                ),
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x68))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_automatic_self_calibration(config[CONF_AUTOMATIC_SELF_CALIBRATION]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

    if CONF_CO2 in config:
        sens = await sensor.new_sensor(config[CONF_CO2])
        cg.add(var.set_co2_sensor(sens))
