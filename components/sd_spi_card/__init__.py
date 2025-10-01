import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins, automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@IEQLab"]
DEPENDENCIES = []

CONF_ON_MOUNT = "on_mount"
CONF_AUTO_MOUNT = "auto_mount"

sd_ns = cg.esphome_ns.namespace("sd_spi_card")
SdSpiCard = sd_ns.class_("SdSpiCard", cg.Component)

# Triggers
SdSpiCardMountTrigger = sd_ns.class_("SdSpiCardMountTrigger", automation.Trigger.template())

# Actions
AppendFileAction = sd_ns.class_("AppendFileAction", automation.Action)
WriteFileAction = sd_ns.class_("WriteFileAction", automation.Action)
SyncAction = sd_ns.class_("SyncAction", automation.Action)
CreateFileAction = sd_ns.class_("CreateFileAction", automation.Action)
MountAction = sd_ns.class_("MountAction", automation.Action)

# Define config keys
CONF_PATH = "path"
CONF_CONTENT = "content"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SdSpiCard),
        cv.Required("clk_pin"): pins.internal_gpio_output_pin_number,
        cv.Required("mosi_pin"): pins.internal_gpio_output_pin_number,
        cv.Required("miso_pin"): pins.internal_gpio_input_pin_number,
        cv.Required("cs_pin"): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_AUTO_MOUNT, default=True): cv.boolean,
        cv.Optional(CONF_ON_MOUNT): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(SdSpiCardMountTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pins(
        config["clk_pin"],
        config["mosi_pin"],
        config["miso_pin"],
        config["cs_pin"]
    ))
    
    cg.add(var.set_auto_mount(config[CONF_AUTO_MOUNT]))
    
    # Register on_mount trigger
    for conf in config.get(CONF_ON_MOUNT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


# Append file action
@automation.register_action(
    "sd_spi_card.append_file",
    AppendFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SdSpiCard),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Required(CONF_CONTENT): cv.templatable(cv.string),
        }
    ),
)
async def append_file_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    
    template_ = await cg.templatable(config[CONF_CONTENT], args, cg.std_string)
    cg.add(var.set_content(template_))
    
    return var


# Write file action
@automation.register_action(
    "sd_spi_card.write_file",
    WriteFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SdSpiCard),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Required(CONF_CONTENT): cv.templatable(cv.string),
        }
    ),
)
async def write_file_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    
    template_ = await cg.templatable(config[CONF_CONTENT], args, cg.std_string)
    cg.add(var.set_content(template_))
    
    return var


# Sync action
@automation.register_action(
    "sd_spi_card.sync",
    SyncAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SdSpiCard),
        }
    ),
)
async def sync_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    return var


# Create file action
@automation.register_action(
    "sd_spi_card.create_file",
    CreateFileAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SdSpiCard),
            cv.Required(CONF_PATH): cv.templatable(cv.string),
            cv.Optional(CONF_CONTENT, default=""): cv.templatable(cv.string),
        }
    ),
)
async def create_file_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    
    template_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(template_))
    
    template_ = await cg.templatable(config[CONF_CONTENT], args, cg.std_string)
    cg.add(var.set_content(template_))
    
    return var


# Mount action
@automation.register_action(
    "sd_spi_card.mount",
    MountAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SdSpiCard),
        }
    ),
)
async def mount_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    return var
