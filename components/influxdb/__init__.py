import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import http_request, time
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = ["http_request", "time"]

CONF_HOST = "host"
CONF_TOKEN = "token"
CONF_BUCKET = "bucket"
CONF_ORG = "org"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_TIME_ID = "time_id"
CONF_PORT = "port"
CONF_TIMESTAMP_UNIT = "timestamp_unit"
CONF_TAGS = "sensor_tags"
CONF_GLOBAL_TAGS = "global_tags"
CONF_FIELD_NAME = "field_names"
CONF_USE_SSL = "use_ssl"
CONF_SENSORS_NAMES = "sensor_names"
CONF_SEND_MAC = "send_mac"

influxdb_ns = cg.esphome_ns.namespace("influxdb")
InfluxDB = influxdb_ns.class_("InfluxDB", cg.Component)

def validate_update_interval(value):
    """Validate update interval using ESPHome's built-in validation that supports 'never'."""
    # Use ESPHome's native update_interval validation which handles "never"
    return cv.update_interval(value)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(InfluxDB),
    cv.Required(CONF_HTTP_REQUEST_ID): cv.use_id(http_request.HttpRequestComponent),
    cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Required(CONF_HOST): cv.string_strict,
    cv.Required(CONF_TOKEN): cv.string_strict,
    cv.Required(CONF_BUCKET): cv.string_strict,
    cv.Required(CONF_ORG): cv.string_strict,
    cv.Optional(CONF_PORT, default="8086"): cv.port,
    cv.Required(CONF_SENSORS_NAMES): cv.Schema({cv.string: cv.string}),
    cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): validate_update_interval,
    cv.Optional(CONF_SEND_MAC, default=True): cv.boolean,
    cv.Optional(CONF_USE_SSL, default=True): cv.boolean,
    cv.Optional(CONF_TIMESTAMP_UNIT, default="s"): cv.one_of("s", "ms", "us", "ns", lower=True),
    cv.Optional(CONF_FIELD_NAME, default={}): cv.Schema({
        cv.string: cv.string
    }),
    cv.Optional(CONF_TAGS, default={}): cv.Schema({
        cv.string: cv.Schema({
            cv.string: cv.string
        })
    }),
    cv.Optional(CONF_GLOBAL_TAGS, default={}): cv.Schema({
        cv.string: cv.string
    }),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # HTTP request component
    request = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(request))
    
    # Time component (optional)
    if CONF_TIME_ID in config:
        time_ = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_))
    
    # Basic configuration
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_bucket(config[CONF_BUCKET]))
    cg.add(var.set_org(config[CONF_ORG]))
    cg.add(var.set_port(str(config[CONF_PORT])))
    cg.add(var.set_use_ssl(config[CONF_USE_SSL]))
    cg.add(var.set_timestamp_unit(config[CONF_TIMESTAMP_UNIT]))
    cg.add(var.set_send_mac(config[CONF_SEND_MAC]))
    
    # Handle update interval using ESPHome's standard approach
    # ESPHome's cv.update_interval converts "never" to UINT32_MAX (4294967295)
    update_interval_value = config[CONF_UPDATE_INTERVAL]
    cg.add(var.set_update_interval(update_interval_value))

    # Sensor mappings
    if CONF_SENSORS_NAMES in config:
        for sensor_id, measurement_name in config[CONF_SENSORS_NAMES].items():
            cg.add(var.add_sensor_mapping(sensor_id, measurement_name))
    
    # Global tags (applied to all measurements)
    if CONF_GLOBAL_TAGS in config:
        for tag_key, tag_value in config[CONF_GLOBAL_TAGS].items():
            cg.add(var.add_global_tag(tag_key, tag_value))
    
    # Static tags (per-sensor)
    if CONF_TAGS in config:
        for sensor_id, tags in config[CONF_TAGS].items():
            for tag_key, tag_value in tags.items():
                cg.add(var.add_static_tag(sensor_id, tag_key, tag_value))
    
    # Field names
    if CONF_FIELD_NAME in config:
        for sensor_id, field_name in config[CONF_FIELD_NAME].items():
            cg.add(var.set_field_name(sensor_id, field_name))
