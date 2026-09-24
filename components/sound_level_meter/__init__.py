# pylint: disable=no-name-in-module,invalid-name,unused-argument

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, core
from esphome.automation import maybe_simple_id
from esphome.components import sensor, microphone, ota
from esphome.components.esp32 import add_idf_component
from esphome.const import (
    CONF_ID,
    CONF_SENSORS,
    CONF_WINDOW_SIZE,
    CONF_UPDATE_INTERVAL,
    CONF_TYPE,
    CONF_MICROPHONE,
    UNIT_DECIBEL,
    STATE_CLASS_MEASUREMENT,
    DEVICE_CLASS_SOUND_PRESSURE,
)

CODEOWNERS = ["@stas-sl"]
DEPENDENCIES = ["esp32", "microphone"]
AUTO_LOAD = ["sensor", "audio", "ring_buffer"]
MULTI_CONF = True

sound_level_meter_ns = cg.esphome_ns.namespace("sound_level_meter")
SoundLevelMeter = sound_level_meter_ns.class_("SoundLevelMeter", cg.Component)
SoundLevelMeterProcessor = sound_level_meter_ns.class_("SoundLevelMeterProcessor")
SoundLevelMeterSensor = sound_level_meter_ns.class_(
    "SoundLevelMeterSensor", SoundLevelMeterProcessor, sensor.Sensor
)
SoundLevelMeterStats = sound_level_meter_ns.class_(
    "SoundLevelMeterStats", SoundLevelMeterProcessor
)
SoundLevelMeterSensorEq = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorEq", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorMax = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorMax", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorMin = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorMin", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorPeak = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorPeak", SoundLevelMeterSensor, sensor.Sensor
)
Filter = sound_level_meter_ns.class_("Filter")
SOS_Filter = sound_level_meter_ns.class_("SOS_Filter", Filter)
StartAction = sound_level_meter_ns.class_("StartAction", automation.Action)
StopAction = sound_level_meter_ns.class_("StopAction", automation.Action)


CONF_EQ = "eq"
CONF_MAX = "max"
CONF_MIN = "min"
CONF_PEAK = "peak"
CONF_STATS = "stats"
CONF_WINDOW = "window"
CONF_SEND_EVERY = "send_every"
CONF_SEND_FIRST_AT = "send_first_at"
CONF_LEQ = "leq"
CONF_L90 = "l90"
CONF_L10 = "l10"
CONF_L05 = "l05"
STATS_SENSORS = (CONF_LEQ, CONF_L90, CONF_L10, CONF_L05)
CONF_RING_BUFFER_SIZE = "ring_buffer_size"
CONF_SOS = "sos"
CONF_COEFFS = "coeffs"
CONF_WARMUP_INTERVAL = "warmup_interval"
CONF_TASK_STACK_SIZE = "task_stack_size"
CONF_TASK_PRIORITY = "task_priority"
CONF_TASK_CORE = "task_core"
CONF_MIC_SENSITIVITY = "mic_sensitivity"
CONF_MIC_SENSITIVITY_REF = "mic_sensitivity_ref"
CONF_OFFSET = "offset"
CONF_HIGH_FREQ = "high_freq"
CONF_DSP_FILTERS = "dsp_filters"
CONF_AUTO_START = "auto_start"
CONF_USE_ESP_DSP = "use_esp_dsp"

ICON_WAVEFORM = "mdi:waveform"

CONFIG_DSP_FILTER_SCHEMA = cv.typed_schema(
    {
        CONF_SOS: cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(SOS_Filter),
                cv.Required(CONF_COEFFS): [[cv.float_]],
            }
        )
    }
)

CONFIG_SENSOR_DSP_FILTER_SCHEMA = cv.ensure_list(
    cv.Any(cv.use_id(Filter), CONFIG_DSP_FILTER_SCHEMA)
)

STATS_LEVEL_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_DECIBEL,
    accuracy_decimals=1,
    state_class=STATE_CLASS_MEASUREMENT,
    device_class=DEVICE_CLASS_SOUND_PRESSURE,
    icon=ICON_WAVEFORM,
)


def _blocks(config, key):
    """A duration as a whole number of update_interval blocks."""
    block = config[CONF_UPDATE_INTERVAL].total_milliseconds
    period = config[key].total_milliseconds
    if period % block != 0:
        raise cv.Invalid(f"{key} must be a multiple of update_interval ({block}ms)", [key])
    blocks = period // block
    if not 1 <= blocks <= 65535:
        raise cv.Invalid(f"{key} must be 1 to 65535 blocks of update_interval, got {blocks}", [key])
    return blocks


def _validate_stats(config):
    config = config.copy()
    config.setdefault(CONF_SEND_FIRST_AT, config[CONF_SEND_EVERY])
    for key in (CONF_WINDOW, CONF_SEND_EVERY, CONF_SEND_FIRST_AT):
        _blocks(config, key)
    return config


CONFIG_SENSOR_SCHEMA = cv.typed_schema(
    {
        CONF_EQ: sensor.sensor_schema(
            SoundLevelMeterSensorEq,
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            device_class=DEVICE_CLASS_SOUND_PRESSURE,
            icon=ICON_WAVEFORM,
        ).extend(
            {
                cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
                cv.Optional(
                    CONF_DSP_FILTERS, default=[]
                ): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
            }
        ),
        CONF_MAX: sensor.sensor_schema(
            SoundLevelMeterSensorMax,
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            device_class=DEVICE_CLASS_SOUND_PRESSURE,
            icon=ICON_WAVEFORM,
        ).extend(
            {
                cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
                cv.Required(CONF_WINDOW_SIZE): cv.positive_time_period_milliseconds,
                cv.Optional(
                    CONF_DSP_FILTERS, default=[]
                ): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
            }
        ),
        CONF_MIN: sensor.sensor_schema(
            SoundLevelMeterSensorMin,
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            device_class=DEVICE_CLASS_SOUND_PRESSURE,
            icon=ICON_WAVEFORM,
        ).extend(
            {
                cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
                cv.Required(CONF_WINDOW_SIZE): cv.positive_time_period_milliseconds,
                cv.Optional(
                    CONF_DSP_FILTERS, default=[]
                ): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
            }
        ),
        CONF_PEAK: sensor.sensor_schema(
            SoundLevelMeterSensorPeak,
            unit_of_measurement=UNIT_DECIBEL,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            device_class=DEVICE_CLASS_SOUND_PRESSURE,
            icon=ICON_WAVEFORM,
        ).extend(
            {
                cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
                cv.Optional(
                    CONF_DSP_FILTERS, default=[]
                ): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
            }
        ),
        # Leq and level quantiles over a sliding window of update_interval blocks, with fixed
        # memory: 2 bytes per block plus a 2.8KB histogram, allocated once in setup()
        CONF_STATS: cv.All(
            cv.Schema(
                {
                    cv.GenerateID(): cv.declare_id(SoundLevelMeterStats),
                    cv.Optional(
                        CONF_UPDATE_INTERVAL, default="125ms"
                    ): cv.positive_time_period_milliseconds,
                    cv.Required(CONF_WINDOW): cv.positive_time_period_milliseconds,
                    cv.Required(CONF_SEND_EVERY): cv.positive_time_period_milliseconds,
                    cv.Optional(CONF_SEND_FIRST_AT): cv.positive_time_period_milliseconds,
                    cv.Optional(
                        CONF_DSP_FILTERS, default=[]
                    ): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
                    **{cv.Optional(key): STATS_LEVEL_SCHEMA for key in STATS_SENSORS},
                }
            ),
            cv.has_at_least_one_key(*STATS_SENSORS),
            _validate_stats,
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SoundLevelMeter),
            cv.Optional(
                CONF_MICROPHONE, default={}
            ): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=32,
            ),
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
            cv.Optional(CONF_HIGH_FREQ, default=False): cv.boolean,
            cv.Optional(
                CONF_RING_BUFFER_SIZE, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_WARMUP_INTERVAL, default="0ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_STACK_SIZE, default=4096): cv.positive_not_null_int,
            cv.Optional(CONF_TASK_PRIORITY, default=2): cv.uint8_t,
            cv.Optional(CONF_TASK_CORE, default=1): cv.int_range(0, 1),
            cv.Optional(CONF_MIC_SENSITIVITY): cv.decibel,
            cv.Optional(CONF_MIC_SENSITIVITY_REF): cv.decibel,
            cv.Optional(CONF_OFFSET): cv.decibel,
            cv.Optional(CONF_DSP_FILTERS, default=[]): [CONFIG_DSP_FILTER_SCHEMA],
            cv.Optional(CONF_SENSORS, default=[]): [CONFIG_SENSOR_SCHEMA],
            cv.Optional(CONF_USE_ESP_DSP, default=False): cv.All(
                cv.boolean
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)

SOUND_LEVEL_METER_ACTION_SCHEMA = maybe_simple_id(
    {cv.GenerateID(): cv.use_id(SoundLevelMeter)}
)


async def add_dsp_filter(config, parent):
    f = None
    if config[CONF_TYPE] == CONF_SOS:
        f = cg.new_Pvariable(config[CONF_ID], config[CONF_COEFFS])
    assert f is not None
    cg.add(parent.add_dsp_filter(f))
    return f


async def add_sensor(config, parent):
    if config[CONF_TYPE] == CONF_STATS:
        s = cg.new_Pvariable(config[CONF_ID])
        cg.add(s.set_window_blocks(_blocks(config, CONF_WINDOW)))
        cg.add(s.set_send_every(_blocks(config, CONF_SEND_EVERY)))
        cg.add(s.set_send_first_at(_blocks(config, CONF_SEND_FIRST_AT)))
        for key in STATS_SENSORS:
            if (sc := config.get(key)) is not None:
                cg.add(getattr(s, f"set_{key}_sensor")(await sensor.new_sensor(sc)))
    else:
        s = await sensor.new_sensor(config)
    cg.add(s.set_parent(parent))
    if CONF_WINDOW_SIZE in config:
        cg.add(s.set_window_size(config[CONF_WINDOW_SIZE]))
    if CONF_UPDATE_INTERVAL in config:
        cg.add(s.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    for fc in config[CONF_DSP_FILTERS]:
        f = None
        if isinstance(fc, core.ID):
            f = await cg.get_variable(fc)
        elif isinstance(fc, dict):
            f = await add_dsp_filter(fc, parent)
        assert f is not None
        cg.add(s.add_dsp_filter(f))
    cg.add(parent.add_processor(s))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=False
    )
    cg.add(var.set_microphone_source(mic_source))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_ring_buffer_size(config[CONF_RING_BUFFER_SIZE]))
    cg.add(var.set_warmup_interval(config[CONF_WARMUP_INTERVAL]))
    cg.add(var.set_task_stack_size(config[CONF_TASK_STACK_SIZE]))
    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))
    cg.add(var.set_is_high_freq(config[CONF_HIGH_FREQ]))
    cg.add(var.set_is_auto_start(config[CONF_AUTO_START]))
    if CONF_MIC_SENSITIVITY in config:
        cg.add(var.set_mic_sensitivity(config[CONF_MIC_SENSITIVITY]))
    if CONF_MIC_SENSITIVITY_REF in config:
        cg.add(var.set_mic_sensitivity_ref(config[CONF_MIC_SENSITIVITY_REF]))
    if CONF_OFFSET in config:
        cg.add(var.set_offset(config[CONF_OFFSET]))
    if config[CONF_USE_ESP_DSP]:
        add_idf_component(name="espressif/esp-dsp", ref="1.7.0")
        cg.add_define("USE_ESP_DSP")

    for fc in config[CONF_DSP_FILTERS]:
        await add_dsp_filter(fc, var)

    for sc in config[CONF_SENSORS]:
        await add_sensor(sc, var)

    ota.request_ota_state_listeners()


@automation.register_action(
    "sound_level_meter.start", StartAction, SOUND_LEVEL_METER_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "sound_level_meter.stop", StopAction, SOUND_LEVEL_METER_ACTION_SCHEMA,
    synchronous=True,
)
async def switch_toggle_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
