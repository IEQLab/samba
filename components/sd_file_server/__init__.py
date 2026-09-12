import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.sd_spi_card import SdSpiCard
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID
from esphome.core import coroutine_with_priority
from esphome.coroutine import CoroPriority

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = ["esp32", "sd_spi_card"]
AUTO_LOAD = ["web_server_base"]

CONF_SD_SPI_CARD_ID = "sd_spi_card_id"

sd_file_server_ns = cg.esphome_ns.namespace("sd_file_server")
SdFileServer = sd_file_server_ns.class_("SdFileServer", cg.Component)


def _consume_sockets(config):
    # the phone keeps one keep-alive connection; the listening socket is web_server_base's
    from esphome.components import socket

    socket.consume_sockets(1, "sd_file_server")(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SdFileServer),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Required(CONF_SD_SPI_CARD_ID): cv.use_id(SdSpiCard),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_sockets,
)


@coroutine_with_priority(CoroPriority.CAPTIVE_PORTAL)
async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    card = await cg.get_variable(config[CONF_SD_SPI_CARD_ID])
    # Both dependencies go through the constructor: nothing is wired after registration, so
    # safe mode's early return in setup() cannot leave this half-built (see CLAUDE.md).
    var = cg.new_Pvariable(config[CONF_ID], base, card)
    await cg.register_component(var, config)
    # Turns on web_server_base's auth middleware, digest flavour. The credentials themselves
    # are set at run time by the component; with no password the middleware passes everything.
    cg.add_define("USE_WEBSERVER_AUTH")
    cg.add_define("USE_WEBSERVER_AUTH_DIGEST")
