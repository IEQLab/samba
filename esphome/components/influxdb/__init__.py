import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT, CONF_USERNAME, CONF_PASSWORD
from esphome.core import coroutine_with_priority
from esphome.core import CORE

DEPENDENCIES = ['network']
AUTO_LOAD = ['http_request']

influxdb_ns = cg.esphome_ns.namespace('influxdb')
InfluxDBWriter = influxdb_ns.class_('InfluxDBWriter', cg.Component, cg.Controller)

CONF_HOST = 'host'
CONF_USE_SSL = 'use_ssl'
CONF_BUCKET = 'bucket'
CONF_TOKEN = 'token'
CONF_ORG = 'org'
CONF_ORGID = 'orgid'
CONF_PRECISION = 'precision'
CONF_DATABASE = 'database'
CONF_SEND_TIMEOUT = 'send_timeout'
CONF_TAGS = 'tags'
CONF_PUBLISH_ALL = 'publish_all'
CONF_SENSORS = 'sensors'
CONF_IGNORE = 'ignore'
CONF_MEASUREMENT = 'measurement'


SENSOR_SCHEMA = cv.Schema({
   cv.validate_id_name:
       cv.Schema({
       cv.Optional(CONF_IGNORE, default=False): cv.boolean,
       cv.Optional(CONF_MEASUREMENT): cv.string,
       cv.Optional(CONF_TAGS, default={}): cv.Schema({
           cv.string: cv.string
       })
   })
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(InfluxDBWriter),
    cv.Required(CONF_HOST): cv.domain,
    cv.Required(CONF_BUCKET): cv.string_strict,
    cv.Required(CONF_TOKEN): cv.string_strict,
    cv.Optional(CONF_USE_SSL, default=True): cv.boolean,
    cv.Optional(CONF_ORG, default=''): cv.string_strict,
    cv.Optional(CONF_ORGID, default=''): cv.string_strict,
    cv.Optional(CONF_PRECISION, default='ns'): cv.string_strict,
    cv.Optional(CONF_PORT, default=8086): cv.port,
    cv.Optional(CONF_DATABASE , default='esphome'): cv.string_strict,
    cv.Optional(CONF_SEND_TIMEOUT, default='500ms'): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_PUBLISH_ALL, default=True): cv.boolean,
    cv.Optional(CONF_TAGS, default={'node': CORE.name}): cv.Schema({
        cv.valid_name: cv.valid_name
    }),
    cv.Optional(CONF_SENSORS, default={}): SENSOR_SCHEMA,
}).extend(cv.COMPONENT_SCHEMA)


@coroutine_with_priority(40)
def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    yield cg.register_component(var, config)

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_use_ssl(config[CONF_USE_SSL]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_bucket(config[CONF_BUCKET]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_org(config[CONF_ORG]))
    cg.add(var.set_orgId(config[CONF_ORGID]))
    cg.add(var.set_precision(config[CONF_PRECISION]))
    cg.add(var.set_send_timeout(config[CONF_SEND_TIMEOUT]))
    cg.add(var.set_publish_all(config[CONF_PUBLISH_ALL]))

    cg.add(var.set_tags(''.join(',{}={}'.format(tag, value) for tag, value in config[CONF_TAGS].items())))

    for sensor_id, sensor_config in config[CONF_SENSORS].items():
        if sensor_config[CONF_IGNORE] == False:
            tags = ''.join(',{}={}'.format(tag, value) for tag, value in {**config[CONF_TAGS] , **sensor_config[CONF_TAGS]}.items())
            if 'measurement' in sensor_config:
                measurement = f"\"{sensor_config[CONF_MEASUREMENT]}\""
            else:
                measurement = f"{sensor_id}->get_object_id()"
            cg.add(var.add_setup_callback(cg.RawExpression(f"[]() -> EntityBase* {{ {sensor_id}->add_on_state_callback([](float state) {{ {config[CONF_ID]}->on_sensor_update({sensor_id}, {measurement}, \"{tags}\", state); }}); return {sensor_id}; }}")))
        else:
            cg.add(var.add_setup_callback(cg.RawExpression(f"[]() -> EntityBase* {{ return {sensor_id}; }}")))

    cg.add_define('USE_INFLUXDB')
    cg.add_global(influxdb_ns.using)
