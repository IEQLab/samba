import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import http_request
from esphome.components import sntp 
from esphome.const import CONF_ID

CODEOWNERS = ["@IgnacioCipo"]
DEPENDENCIES = ["http_request", "time", "binary_sensor", "text_sensor"]

CONF_HOST = "host"
CONF_TOKEN = "token"
CONF_BUCKET = "bucket"
CONF_ORG = "org"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_TIME_ID = "time_id"
CONF_PORT = "port"
CONF_TIMESTAMP_UNIT = "timestamp_unit"
CONF_TAGS = "sensor_tags"
CONF_FIELD_NAME = "field_names"
CONF_USE_SSL = "use_ssl"
CONF_SENSORS_NAMES_ID = "sensors_names"
CONF_SEND_MAC = "send_mac"

influxdb_writer_ns = cg.esphome_ns.namespace("influxdb_writer")
InfluxDBWriter = influxdb_writer_ns.class_("InfluxDBWriter", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(InfluxDBWriter),
    cv.Required(CONF_HTTP_REQUEST_ID): cv.use_id(http_request.HttpRequestComponent),
    cv.Required(CONF_TIME_ID): cv.use_id(sntp),
    cv.Required(CONF_HOST): cv.string,
    cv.Required(CONF_TOKEN): cv.string,
    cv.Required(CONF_BUCKET): cv.string,
    cv.Required(CONF_ORG): cv.string,
    cv.Required(CONF_PORT): cv.string,
    cv.Required(CONF_SENSORS_NAMES_ID): cv.Schema({cv.string: cv.string}),
    cv.Optional(CONF_SEND_MAC, default=True) : cv.boolean,
    cv.Optional(CONF_USE_SSL, default=True): cv.boolean,
    cv.Optional(CONF_TIMESTAMP_UNIT, default="s"): cv.one_of("s", "ms", lower=True),
    cv.Optional(CONF_FIELD_NAME, default={}): cv.Schema({
        cv.string: cv.string
    }),
    cv.Optional(CONF_TAGS, default={}): cv.Schema({
        cv.string: cv.Schema({
            cv.string: cv.string
        })
    }),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    request = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    time_ = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(time_))
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_bucket(config[CONF_BUCKET]))
    cg.add(var.set_org(config[CONF_ORG]))
    cg.add(var.set_use_ssl(config[CONF_USE_SSL]))
    cg.add(var.set_http_request(request))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_influxdb_timestamp_unit(config[CONF_TIMESTAMP_UNIT]))
    cg.add(var.set_send_mac(config[CONF_SEND_MAC]))
    if CONF_TAGS in config:
        for sensor_id, tags in config[CONF_TAGS].items():
            for tag_key, tag_value in tags.items():
                cg.add(var.add_sensor_tag(sensor_id, tag_key, tag_value))
    if CONF_FIELD_NAME in config:
        for sensor_id, field_name in config[CONF_FIELD_NAME].items():
            cg.add(var.set_field_name(sensor_id, field_name))
    if CONF_SENSORS_NAMES_ID in config:
        for sensor_id, name in config[CONF_SENSORS_NAMES_ID].items():
            cg.add(var.add_sensor_name_id(sensor_id, name))





    
    

