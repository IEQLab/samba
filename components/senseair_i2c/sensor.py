"""
Senseair I2C CO₂ Sensor Platform for ESPHome
- Supports automatic baseline correction (ABC) configuration on boot.
- ABC settings are written to EEPROM and require power cycle to take effect.
- Allows YAML configuration of retry/timing parameters for reliability.
- Validates ABC interval to prevent overflow (max 65535 hours).
"""

from esphome import core, automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_I2C_ID,
    CONF_ID,
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
CONF_READ_DELAY_MS = "read_delay_ms"

# ABC interval limits (register is 16-bit, units in hours)
ABC_INTERVAL_MAX_HOURS = 65535
ABC_INTERVAL_MAX_SECONDS = ABC_INTERVAL_MAX_HOURS * 3600  # ~7.5 years

# Actions
BackgroundCalibrationAction = senseair_i2c_ns.class_(
    "BackgroundCalibrationAction", automation.Action
)
ABCGetPeriodAction = senseair_i2c_ns.class_(
    "ABCGetPeriodAction", automation.Action
)


def validate_abc_interval(value):
    """Validate ABC interval is within sensor limits.
    
    Supports:
    - "never" - disables ABC
    - "0s", "0h", "0m" etc - any zero time period disables ABC
    - Valid time periods up to 65535 hours
    """
    # Support "never" as explicit disable
    if value == "never":
        return "0s"
    
    # Support "0s" directly
    if value == "0s":
        return value
    
    # Parse as time period using ESPHome's validator
    # This handles all time formats like "180h", "0h", etc.
    time_period = cv.positive_time_period_seconds(value)
    seconds = int(time_period.total_seconds)
    
    # Check if it's effectively zero (treat as disable)
    if seconds == 0:
        return "0s"
    
    # Validate interval is within limits
    hours = seconds / 3600
    if hours > ABC_INTERVAL_MAX_HOURS:
        raise cv.Invalid(
            f"ABC interval too large: {hours:.1f} hours. "
            f"Maximum is {ABC_INTERVAL_MAX_HOURS} hours ({ABC_INTERVAL_MAX_HOURS/24:.1f} days). "
            f"The sensor uses a 16-bit register (units: hours)."
        )
    
    return time_period


CONFIG_SCHEMA = (
    sensor.sensor_schema(
        SenseairI2CSensor,
        unit_of_measurement=UNIT_PARTS_PER_MILLION,
        icon=ICON_MOLECULE_CO2,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_CARBON_DIOXIDE,
        state_class=STATE_CLASS_MEASUREMENT,
    )
    .extend(
        {
            cv.Optional(CONF_I2C_ID): cv.use_id(i2c.I2CBus),
            cv.Optional(CONF_ADDRESS, default=0x68): cv.i2c_address,
            # ABC interval: "0s", "0h", "never" disables ABC, otherwise sets interval
            # Maximum 65535 hours due to 16-bit register limitation
            cv.Optional(CONF_ABC_INTERVAL, default="180h"): validate_abc_interval,
            # Robustness options
            cv.Optional(CONF_RETRY_DELAY_MS, default=200): cv.int_range(min=10, max=5000),
            cv.Optional(CONF_MAX_RETRIES, default=5): cv.int_range(min=1, max=20),
            cv.Optional(CONF_READ_DELAY_MS, default=50): cv.int_range(min=10, max=1000),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x68))
)


async def to_code(config):
    """Generate C++ code for the Senseair I2C CO₂ sensor."""
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    # Set automatic baseline correction (ABC) interval
    abc_interval = config[CONF_ABC_INTERVAL]
    abc_seconds = 0 if abc_interval == "0s" else int(abc_interval.total_seconds)
    cg.add(var.set_abc_interval(abc_seconds))

    # Set retry parameters
    cg.add(var.set_retry_delay_ms(config[CONF_RETRY_DELAY_MS]))
    cg.add(var.set_max_retries(config[CONF_MAX_RETRIES]))
    cg.add(var.set_read_delay_ms(config[CONF_READ_DELAY_MS]))


@automation.register_action(
    "senseair_i2c.background_calibration",
    BackgroundCalibrationAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SenseairI2CSensor),
        }
    ),
)
async def background_calibration_action_to_code(config, action_id, template_arg, args):
    """Register background calibration action."""
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "senseair_i2c.abc_get_period",
    ABCGetPeriodAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SenseairI2CSensor),
        }
    ),
)
async def abc_get_period_action_to_code(config, action_id, template_arg, args):
    """Register ABC get period action."""
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)