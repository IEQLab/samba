import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button, switch, web_server_base
from esphome.components.sd_spi_card import SdSpiCard
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID
from esphome.core import coroutine_with_priority
from esphome.coroutine import CoroPriority

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = ["esp32", "sd_spi_card"]
AUTO_LOAD = ["web_server_base", "switch", "button"]

CONF_SD_SPI_CARD_ID = "sd_spi_card_id"
CONF_SETTINGS = "settings"
CONF_ACTIONS = "actions"
CONF_SWITCH = "switch"
CONF_AVAILABLE = "available"
MAX_ENTRIES = 8  # SdFileServer::MAX_ENTRIES; a bit each in a uint8_t

sd_file_server_ns = cg.esphome_ns.namespace("sd_file_server")
SdFileServer = sd_file_server_ns.class_("SdFileServer", cg.Component)


def _consume_sockets(config):
    # the phone keeps one keep-alive connection; the listening socket is web_server_base's
    from esphome.components import socket

    socket.consume_sockets(1, "sd_file_server")(config)
    return config


def _wire_key(value):
    # Keys are the wire names in home-sync.md section 2: GET /settings keys, /action/<key> paths
    value = cv.string_strict(value)
    if not re.fullmatch(r"[a-z][a-z0-9_]{0,14}", value):
        raise cv.Invalid(f"'{value}': 1 to 15 characters of a-z, 0-9 and _, starting with a letter")
    return value


def _entries(value_schema):
    return cv.All(cv.Schema({_wire_key: value_schema}), cv.Length(max=MAX_ENTRIES))


_SETTING_FULL = cv.Schema(
    {
        cv.Required(CONF_SWITCH): cv.use_id(switch.Switch),
        # false: the setting reads off and turning it on is refused with 409
        cv.Optional(CONF_AVAILABLE): cv.returning_lambda,
    }
)


def _setting(value):
    # A bare switch id, or the full form with an availability lambda
    if isinstance(value, dict):
        return _SETTING_FULL(value)
    return {CONF_SWITCH: cv.use_id(switch.Switch)(value)}


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SdFileServer),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Required(CONF_SD_SPI_CARD_ID): cv.use_id(SdSpiCard),
            # Switches readable and settable at /settings, buttons pressable at /action/<key>
            cv.Optional(CONF_SETTINGS, default={}): _entries(_setting),
            cv.Optional(CONF_ACTIONS, default={}): _entries(cv.use_id(button.Button)),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_sockets,
)


@coroutine_with_priority(CoroPriority.CAPTIVE_PORTAL)
async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    card = await cg.get_variable(config[CONF_SD_SPI_CARD_ID])
    settings = []
    for key, conf in config[CONF_SETTINGS].items():
        args = [key, await cg.get_variable(conf[CONF_SWITCH])]
        if CONF_AVAILABLE in conf:
            args.append(await cg.process_lambda(conf[CONF_AVAILABLE], [], return_type=cg.bool_))
        settings.append(args)
    actions = [(key, await cg.get_variable(id_)) for key, id_ in config[CONF_ACTIONS].items()]
    # Every dependency is resolved above, so construction, wiring and registration are emitted
    # together with no await between them. The base and card go through the constructor and the
    # entities are added before registration: safe mode's early return in setup() can leave this
    # unregistered but never half-wired (see CLAUDE.md).
    var = cg.new_Pvariable(config[CONF_ID], base, card)
    for args in settings:
        cg.add(var.add_setting(*args))
    for key, btn in actions:
        cg.add(var.add_action(key, btn))
    await cg.register_component(var, config)
    # Turns on web_server_base's auth middleware, digest flavour. The credentials themselves
    # are set at run time by the component; with no password the middleware passes everything.
    cg.add_define("USE_WEBSERVER_AUTH")
    cg.add_define("USE_WEBSERVER_AUTH_DIGEST")
