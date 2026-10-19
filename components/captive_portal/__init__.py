# Local copy of ESPHome 2026.9.1 components/captive_portal, changed only to add WPA2-Enterprise to the portal
# form (captive_portal.h). Re-diff against upstream on every ESPHome upgrade.
import logging

import esphome.codegen as cg
from esphome.components import web_server_base, wifi
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.config_helpers import filter_source_files_from_platform
import esphome.config_validation as cv
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.const import (
    CONF_AP,
    CONF_ID,
    PLATFORM_ESP32,
    PlatformFramework,
)
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority
import esphome.final_validate as fv
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)


def AUTO_LOAD() -> list[str]:
    auto_load = ["web_server_base", "ota.web_server"]
    if CORE.is_esp32:
        auto_load.append("socket")
    return auto_load


DEPENDENCIES = ["wifi"]
CODEOWNERS = ["@esphome/core"]

captive_portal_ns = cg.esphome_ns.namespace("captive_portal")
CaptivePortal = captive_portal_ns.class_("CaptivePortal", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CaptivePortal),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # Enterprise support is ESP-IDF code
    cv.only_on([PLATFORM_ESP32]),
)


def _final_validate(config: ConfigType) -> None:
    full_config = fv.full_config.get()
    wifi_conf = full_config.get("wifi")

    if wifi_conf is None:
        # This shouldn't happen due to DEPENDENCIES = ["wifi"], but check anyway
        raise cv.Invalid("Captive portal requires the wifi component to be configured")

    if CONF_AP not in wifi_conf:
        _LOGGER.warning(
            "Captive portal is enabled but no WiFi AP is configured. "
            "The captive portal will not be accessible. "
            "Add 'ap:' to your WiFi configuration to enable the captive portal."
        )

    # Register socket needs for DNS server and additional HTTP connections
    # - 1 UDP socket for DNS server
    # - 3 TCP sockets for captive portal detection probes + configuration requests
    #   OS captive portal detection makes multiple probe requests that stay in TIME_WAIT.
    #   Need headroom for actual user configuration requests.
    #   LRU purging will reclaim idle sockets to prevent exhaustion from repeated attempts.
    # The listening socket is registered by web_server_base (shared HTTP server).
    from esphome.components import socket

    socket.consume_sockets(3, "captive_portal")(config)
    socket.consume_sockets(1, "captive_portal", socket.SocketType.UDP)(config)


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.CAPTIVE_PORTAL)
async def to_code(config: ConfigType) -> None:
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])

    var = cg.new_Pvariable(config[CONF_ID], paren)
    await cg.register_component(var, config)
    cg.add_define("USE_CAPTIVE_PORTAL")
    # The portal reads wifi scan results from the web server task; this makes the
    # wifi component guard them with a lock on multi-threaded platforms.
    wifi.request_wifi_scan_results_lock()

    # captive_index.h is built from captive_index.html by build_index.py, gzip only
    cg.add_define("USE_CAPTIVE_PORTAL_GZIP")

    # Compiles the enterprise path in wifi without a YAML network, which would key WiFi's saved
    # record to the config hash and lose it on every OTA
    cg.add_define("USE_WIFI_WPA2_EAP")
    CORE.add_job(_enable_enterprise_support)


# wifi's to_code sets this false when no YAML network has eap:, so it must run after it
@coroutine_with_priority(CoroPriority.FINAL)
async def _enable_enterprise_support() -> None:
    add_idf_sdkconfig_option("CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT", True)


# Only compile the ESP-IDF DNS server when using ESP-IDF framework
FILTER_SOURCE_FILES = filter_source_files_from_platform(
    {
        "dns_server_esp32_idf.cpp": {
            PlatformFramework.ESP32_ARDUINO,
            PlatformFramework.ESP32_IDF,
        },
    }
)
