"""
Senseair I2C CO₂ Sensor Platform for ESPHome

Exposes a reliable, robust Senseair K30/K33-family I2C CO₂ sensor to ESPHome.
- Supports automatic baseline correction (ABC) configuration on boot.
- Allows YAML configuration of retry/timing parameters for reliability.
"""

from esphome import core
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_NAME,
    CONF_ADDRESS,
    CONF_I2C_ID,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_CARBON_DIOXIDE,
    ICON_MOLECULE_CO2,
    STATE_CLASS_MEASUREMENT,
    UNIT_PARTS_PER_MILLION,
)

DEPENDENCIES = ["i2c"]

# Component namespace and class registration
senseair_i2c_ns = cg.esphome_ns.namespace("senseair_i2c")
SenseairI2CSensor = senseair_i2c_ns.class_(
    "SenseairI2CSensor", sensor.Sensor, cg.PollingComponent, i2c.I2CDevice
)

# YAML config options
CONF_ABC_INTERVAL = "abc_interval"
CONF_RETRY_DELAY_MS = "retry_delay_ms"
CONF_MAX_RETRIES = "max_retries"

CONFIG_SCHEMA = sensor.sensor_schema(
    SenseairI2CSensor,
    unit_of_measurement=UNIT_PARTS_PER_MILLION,
    icon=ICON_MOLECULE_CO2,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_CARBON_DIOXIDE,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.Optional(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
        cv.Optional(CONF_ADDRESS, default=0x68): cv.i2c_address,
        # ABC interval: disables ABC at 0s, sets interval in seconds otherwise
        cv.Optional(CONF_ABC_INTERVAL, default="24h"): cv.All(
            cv.positive_time_period,
            cv.Range(min=core.TimePeriod(seconds=0), max=core.TimePeriod(days=60)),
        ),
        # Sensor polling (not ABC interval!)
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_seconds,
            cv.Range(
                min=core.TimePeriod(seconds=1),
                max=core.TimePeriod(seconds=1800),
            ),
        ),
        # Robustness options
        cv.Optional(CONF_RETRY_DELAY_MS, default=200): cv.positive_int,
        cv.Optional(CONF_MAX_RETRIES, default=5): cv.positive_int,
    }
)

async def to_code(config):
    """Generate C++ code for the Senseair I2C CO₂ sensor."""
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    # Set automatic baseline correction (ABC) interval
    interval = config[CONF_ABC_INTERVAL]
    seconds = interval.total_seconds if hasattr(interval, "total_seconds") else int(interval)
    cg.add(var.set_abc_interval(seconds))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_retry_delay_ms(config[CONF_RETRY_DELAY_MS]))
    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))