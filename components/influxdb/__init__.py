import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, core
from esphome.components import http_request, sensor, time
from esphome.const import CONF_ID, CONF_PORT, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = ["http_request", "network", "sensor", "time"]

CONF_HOST = "host"
CONF_TOKEN = "token"
CONF_BUCKET = "bucket"
CONF_ORG = "org"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_TIME_ID = "time_id"
CONF_TIMESTAMP_UNIT = "timestamp_unit"
CONF_USE_SSL = "use_ssl"
CONF_SEND_MAC = "send_mac"
CONF_SENSORS = "sensors"
CONF_MEASUREMENT = "measurement"
CONF_FIELD = "field"
CONF_GLOBAL_TAGS = "global_tags"
CONF_SENSOR_IDS = "sensor_ids"

# Must match InfluxDB::MAX_MEASUREMENTS in influxdb.h
MAX_MEASUREMENTS = 4

influxdb_ns = cg.esphome_ns.namespace("influxdb")
InfluxDB = influxdb_ns.class_("InfluxDB", cg.Component)
PublishAction = influxdb_ns.class_("PublishAction", automation.Action)


def line_protocol_key(value):
    """Measurement, field or tag key: emitted verbatim, so it must need no escaping."""
    value = cv.string_strict(value)
    if not value:
        raise cv.Invalid("must not be empty")
    if value.startswith("_"):
        raise cv.Invalid("names starting with '_' are reserved by InfluxDB")
    if any(c in value for c in ' ,="\\') or not value.isprintable():
        raise cv.Invalid(
            "must not contain spaces, commas, '=', quotes, backslashes or control characters"
        )
    return value


def tag_value(value):
    """Static tag value. Spaces, commas and '=' are escaped on the device."""
    value = cv.string_strict(value)
    if not value or not value.isprintable():
        raise cv.Invalid("must be a non-empty printable string")
    return value


SENSOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(sensor.Sensor),
        cv.Required(CONF_MEASUREMENT): line_protocol_key,
        cv.Required(CONF_FIELD): line_protocol_key,
    }
)


def validate_sensors(sensors):
    ids = set()
    fields = set()
    measurements = set()
    for entry in sensors:
        sensor_id = entry[CONF_ID].id
        if sensor_id in ids:
            raise cv.Invalid(f"Sensor '{sensor_id}' is listed more than once")
        ids.add(sensor_id)
        key = (entry[CONF_MEASUREMENT], entry[CONF_FIELD])
        if key in fields:
            raise cv.Invalid(
                f"Field '{key[1]}' is used twice in measurement '{key[0]}'; one value would overwrite the other"
            )
        fields.add(key)
        measurements.add(entry[CONF_MEASUREMENT])
    if len(measurements) > MAX_MEASUREMENTS:
        raise cv.Invalid(
            f"At most {MAX_MEASUREMENTS} distinct measurements are supported, got {len(measurements)}"
        )
    return sensors


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(InfluxDB),
        cv.Required(CONF_HTTP_REQUEST_ID): cv.use_id(http_request.HttpRequestComponent),
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        cv.Required(CONF_HOST): cv.string_strict,
        cv.Required(CONF_TOKEN): cv.sensitive(cv.string_strict),
        cv.Required(CONF_BUCKET): cv.string_strict,
        cv.Required(CONF_ORG): cv.string_strict,
        cv.Optional(CONF_PORT, default=8086): cv.port,
        cv.Required(CONF_SENSORS): cv.All(
            cv.ensure_list(SENSOR_SCHEMA), cv.Length(min=1), validate_sensors
        ),
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.update_interval,
        cv.Optional(CONF_SEND_MAC, default=True): cv.boolean,
        cv.Optional(CONF_USE_SSL, default=True): cv.boolean,
        cv.Optional(CONF_TIMESTAMP_UNIT, default="s"): cv.one_of(
            "s", "ms", "us", "ns", lower=True
        ),
        cv.Optional(CONF_GLOBAL_TAGS, default={}): cv.Schema(
            {line_protocol_key: cv.templatable(tag_value)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_http_request(await cg.get_variable(config[CONF_HTTP_REQUEST_ID])))
    if (time_id := config.get(CONF_TIME_ID)) is not None:
        cg.add(var.set_time_source(await cg.get_variable(time_id)))

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_bucket(config[CONF_BUCKET]))
    cg.add(var.set_org(config[CONF_ORG]))
    cg.add(var.set_use_ssl(config[CONF_USE_SSL]))
    cg.add(var.set_send_mac(config[CONF_SEND_MAC]))
    cg.add(var.set_timestamp_unit(config[CONF_TIMESTAMP_UNIT]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

    for entry in config[CONF_SENSORS]:
        sens = await cg.get_variable(entry[CONF_ID])
        cg.add(var.add_sensor(sens, entry[CONF_MEASUREMENT], entry[CONF_FIELD]))

    for key, value in config[CONF_GLOBAL_TAGS].items():
        if isinstance(value, core.Lambda):
            template = await cg.templatable(value, [], cg.std_string)
            cg.add(var.add_tag(key, template))
        else:
            cg.add(var.add_tag(key, value))


@automation.register_action(
    "influxdb.publish",
    PublishAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(InfluxDB),
            # Omit to publish every configured sensor
            cv.Optional(CONF_SENSOR_IDS): cv.ensure_list(cv.use_id(sensor.Sensor)),
        },
        key=CONF_ID,
    ),
    # play() completes the upload (or schedules its retry) before returning
    synchronous=True,
)
async def publish_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    for sensor_id in config.get(CONF_SENSOR_IDS, []):
        cg.add(var.add_sensor(await cg.get_variable(sensor_id)))
    return var
