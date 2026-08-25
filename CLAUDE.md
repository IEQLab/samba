# CLAUDE.md — SAMBA Project Guide

## Project Overview

SAMBA is an ESPHome-based firmware for indoor environmental quality (IEQ) monitoring, developed by the IEQ Lab at The University of Sydney. It runs on an ESP32 WROOM-32E (16MB flash) using the ESP-IDF framework (not Arduino). The device measures temperature, humidity, globe temperature, air speed, CO2, PM2.5, VOC/NOx, illuminance, and sound pressure level, logging data to InfluxDB, Home Assistant, and SD card.

**Current version:** Check `samba.yaml` for the latest version.
**Min ESPHome version:** 2026.3.0

## Repository Structure

```
samba.yaml              # Main config: ESPHome settings, logger, packages, external components
config/                 # Modular YAML configs (one per function/sensor)
  esp32.yaml            # Board, framework (ESP-IDF), I2C, UART, SPI, sdkconfig
  substitutions.yaml    # Secrets placeholders (WiFi, OTA, InfluxDB credentials)
  globals.yaml          # Persistent calibration coefficients and enable flags
  sample.yaml           # 5-minute sampling loop (sensor update + publish + SD append)
  rtc.yaml              # DS1307 RTC, SNTP sync, sample trigger, firmware check
  sd.yaml               # SD card mount/write via sd_spi_card component
  influx.yaml           # InfluxDB v2 upload config
  homeassistant.yaml    # Native API endpoint
  wifi.yaml             # WiFi and captive portal
  ota.yaml              # HTTP OTA updates
  led.yaml              # WS2812 RGB LED effects
  diagnostics.yaml      # WiFi signal, uptime, restart buttons
  tair.yaml             # SHT4x temperature/RH (linear cal + vapour pressure correction)
  tglobe.yaml           # NTC thermistor globe temperature
  airspeed.yaml         # 2x thermal anemometers (power function cal)
  co2.yaml              # SenseAir K30 via I2C
  pm25.yaml             # Plantower PMS5003 (piecewise RH-corrected cal)
  tvoc.yaml             # Sensirion SGP4x VOC/NOx indices
  illuminance.yaml      # TI OPT3001 lux sensor
  adc.yaml              # ADS1115 analog-to-digital converter
  spl.yaml              # ICS-43434 I2S microphone with DSP (LAeq, LA90, LA10)
components/             # Custom external ESPHome components (C++ and Python)
  sd_spi_card/          # SPI SD card read/write (FAT32, mount at /sd)
  senseair_i2c/         # K30/K33 CO2 sensor over I2C
  influxdb/             # InfluxDB v2 HTTP upload with tags
  sound_level_meter/    # I2S audio DSP for SPL measurement
firmware/               # Compiled binaries, manifest.json for OTA
secrets.yaml            # Credentials (gitignored)
.claude/skills/bump.md   # /bump skill: version bump and release procedure
pcb/                    # Hardware PCB design files
```

## How It Works

### Data Flow (5-minute cycle)

1. Sensors continuously measure at varying intervals (500ms for SPL, up to 60s for VOC).
2. Raw readings pass through filters (clamp, NaN rejection, median smoothing) then calibration lambdas.
3. Every 5 minutes, `sensor_sample` script triggers: updates all template sensors, publishes to InfluxDB, appends CSV row to SD card, blinks LED white.
4. Upload is skipped if device uptime < 2 minutes (warm-up period).

### Calibration

All sensor calibrations use persistent global variables (stored in flash, modifiable via Home Assistant):
- **Linear (y = mx + b):** CO2, temperature, RH, illuminance, globe temp
- **Power (y = a * V^b):** Air speed with temperature compensation
- **Complex:** PM2.5 (piecewise RH-corrected), RH (vapour pressure correction), MRT (radiant heat)

### Error Recovery

- **CO2:** Reinitialize on first failure, restart after 4 consecutive (cyan/blue LED)
- **VOC/NOx:** Restart after 6 consecutive failures (purple/magenta LED), skip during 100s warmup
- **ADS1115:** Warning at 3 min errors (orange LED), restart at 5 min (red LED)
- **System:** Safe mode on boot crash, periodic SD card presence check

### Hardware Pinout

| Bus | Pins | Speed | Devices |
|-----|------|-------|---------|
| I2C Bus A | GPIO25 (SCL), GPIO26 (SDA) | 50kHz | RTC, SHT4x, K30, OPT3001, ADS1115 |
| UART PM | GPIO16 (RX), GPIO17 (TX) | 9600 baud | PMS5003 |
| SPI SD | GPIO18/23/19/5 | — | SD card |
| I2S Audio | GPIO32/33/13 | 48kHz | ICS-43434 microphone |
| LED | GPIO27 | — | WS2812 RGB |

## Development Workflow

### Prerequisites

- ESPHome 2026.3.0+ installed
- `secrets.yaml` with WiFi, OTA, and InfluxDB credentials
- USB-C cable for initial flash

### Common Commands

```bash
# Validate configuration without compiling
esphome config samba.yaml

# Compile firmware
esphome compile samba.yaml

# Compile and flash via USB
esphome run samba.yaml

# View device logs
esphome logs samba.yaml

# OTA flash over WiFi (device must be on network)
esphome run samba.yaml --device <IP_ADDRESS>
```

### Version Bump and Release

Use the `/bump` skill (`.claude/skills/bump.md`):

```
/bump <version> "<summary>" [--tag] [--no-compile] [--dry-run]
```

This compiles the firmware, copies the binary to `firmware/samba_v<version>.bin`, generates an MD5 hash, and updates `manifest.json` for OTA.

**`manifest.json` on `main` is the fleet OTA trigger** — devices poll it every 12h and apply
updates Mondays at 04:00 UTC. Always validate a build on a physical unit before the commit
that updates `manifest.json` lands on `main`.

### OTA Updates

Devices check `firmware/manifest.json` every Monday at 4am. If a new version is available and updates are enabled, the device applies it with random jitter (0-10 min) to avoid fleet-wide simultaneous updates.

## ESPHome Development Guide

### How ESPHome Works

ESPHome uses YAML configuration files to generate C++ firmware for microcontrollers. The Python side parses YAML, validates it, and generates C++ code which is compiled via PlatformIO with the ESP-IDF framework.

Key concepts:
- **Components** are modular units (sensor, switch, light, etc.) with a Python config schema and C++ implementation
- **Packages** allow splitting config across multiple YAML files (as SAMBA does with `config/`)
- **External components** (`components/` dir) extend ESPHome with custom C++ and Python code
- **Lambdas** embed inline C++ in YAML for custom logic (used extensively for calibration)
- **Automations** (scripts, on_time, on_value) define event-driven behavior
- **Globals** store persistent variables in flash memory

### Writing Custom Components

Each component lives in `components/<name>/` with:

```
components/<name>/
├── __init__.py          # Config schema (voluptuous) and code generation
├── <name>.h             # C++ header
├── <name>.cpp           # C++ implementation
└── automation.h         # Optional: custom actions/triggers
```

**Python side (`__init__.py`):**
```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

my_ns = cg.esphome_ns.namespace("my_component")
MyComponent = my_ns.class_("MyComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MyComponent),
    cv.Required("some_param"): cv.string,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_some_param(config["some_param"]))
```

**C++ side:**
```cpp
namespace esphome::my_component {

class MyComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_some_param(const std::string &val) { this->some_param_ = val; }

 protected:
  std::string some_param_;
};

}  // namespace esphome::my_component
```

### C++ Conventions for ESPHome

- Prefix member access with `this->`
- Use 2-space indentation
- Wrap lines at 120 characters
- `lower_snake_case` for functions, methods, variables
- `UpperCamelCase` for classes/structs/enums
- `UPPER_SNAKE_CASE` for global constants
- Trailing underscore for protected/private fields: `value_`
- Prefer `protected` fields over `private` (enables extensibility)
- Use `const Ts &...x` (not `Ts... x`) for action `play()` signatures (ESPHome 2026.x change)

### Embedded Systems Considerations

- **Avoid heap allocation after `setup()`** — fragmentation causes field crashes on long-running devices
- Prefer `std::array` over `std::vector` when size is known at compile time
- Avoid `std::map`, `std::set`, `std::unordered_map` for small datasets (< 16 elements) — use simple structs with linear search
- Never use `std::deque` (allocates 512-byte blocks minimum)
- Be mindful of flash size and RAM usage on ESP32

## SAMBA-Specific Gotchas

### ESPHome 2026.x Compatibility

1. **Missing IDF headers:** ESPHome 2026.x excludes built-in IDF components by default. If you get `esp_vfs_fat.h not found`, add the component to `include_builtin_idf_components` in `config/esp32.yaml`.

2. **Action signature change:** All `play()` overrides must use `const Ts &...x` not `Ts... x`.

3. **FATFS long filenames:** Kconfig "choice" options require explicitly deselecting the default. Both `CONFIG_FATFS_LFN_NONE: "n"` and `CONFIG_FATFS_LFN_HEAP: "y"` must be set. After changing sdkconfig options, delete stale files:
   ```bash
   rm .esphome/build/samba/sdkconfig.samba .esphome/build/samba/sdkconfig.samba.esphomeinternal
   ```

### Never reference an id from `on_safe_mode`

Entering safe mode used to **permanently brick** a unit, and the cause was in our own config.

`safe_mode` calls `App.setup()` and the `on_safe_mode` actions from inside
`should_enter_safe_mode()`, then the generated `setup()` returns early. Anything registered
before that early return but *wired* after it is set up with a null pointer.

Whether the split happens depends on codegen order. ESPHome's scheduler runs a coroutine to its
next `await` then re-queues it at `priority - 1`. `safe_mode` starts at `CoroPriority.APPLICATION`
(50) and ordinary components at 0, so **normally the early return is emitted before any component
and nothing splits**. The bug needs `safe_mode`'s own codegen to wait on something. Ours did:

```yaml
on_safe_mode:
  then:
    - light.turn_on:
        id: samba_led      # waits on cg.get_variable(samba_led)
```

That dropped `safe_mode` below priority 0, so the early return drifted into the middle of the
component block. Five components ended up half-wired; `pmsx003` wrote to a null UART parent and
the three SPL `copy` sensors dereferenced a null source (`LoadProhibited`, `EXCVADDR 0x1c` and
`0x0`). Because `clean_rtc()` saves the boot counter *without* syncing, the crash always landed
before the counter cleared, so every later boot re-entered safe mode and crashed again.

Removing that one action moved the early return from line 2862 to 995 — ahead of all component
registration — leaving **zero** half-wired components. Recovery still works, because logger,
`captive_portal`, `wifi`, the `esphome` OTA platform and the preferences syncer all register
before it. `logger.log` is safe; it waits on nothing.

`ota.http_request.flash` can *never* work here either — `OtaHttpRequestComponent::set_parent()`
always runs after the early return.

`/bump` gates on this, so a regression fails the build instead of bricking devices. To check by
hand, confirm no component registers before the early return but is wired after it:

```bash
esphome compile samba.yaml
python3 - <<'EOF'
import re
L=open('.esphome/build/samba/src/main.cpp').read().split('\n')
cut=next(i for i,l in enumerate(L,1) if 'should_enter_safe_mode' in l)
reg={m.group(1):i for i,l in enumerate(L,1) if (m:=re.search(r'App\.register_component_\((\w+),',l))}
bad=[(v,reg[v],i) for i,l in enumerate(L,1)
     if (m:=re.search(r'(\w+)->set_(parent|source|uart_parent|http_request|request_parent)\b',l))
     and (v:=m.group(1)) in reg and reg[v]<cut<i]
print(f"early return at line {cut}; half-wired: {bad or 'none'}")
EOF
```

### SD Card

- Mount point is `/sd` (hardcoded in sd_spi_card.h)
- Filenames use MAC address + UTC timestamp from DS1307
- `sd_logfile` global flag prevents duplicate file creation per boot
- `script.execute` is async — code after it runs before the script completes

### Logger

- `ESP_LOGCONFIG` (used in `dump_config`) outputs at CONFIG level — set component log level to DEBUG to see it
- Component-specific log levels are set in `samba.yaml` under `logger.logs`

### Secrets

All credentials are in `secrets.yaml` (gitignored) and referenced via `!secret` in substitutions. Never hardcode credentials.

## InfluxDB Architecture

### Consolidated Measurement Schema

The firmware groups all sensor data into 2 InfluxDB measurements instead of 14 separate ones:

- **`ieq`** — all IEQ sensor fields: `air_temp`, `globe_temp`, `rel_humidity`, `air_speed`, `rad_temp`, `co2`, `pm25`, `tvoc`, `nox`, `illuminance`, `la_eq`, `la_90`, `la_10`
- **`device_status`** — diagnostic fields: `wifi`, `uptime`

Tags on both: `building`, `level`, `zone`, `device` (MAC address). Sensors, their measurement and field names are listed in `config/influx.yaml` under `sensors` (keyed by ESPHome id). The C++ grouping logic is in `components/influxdb/influxdb.cpp` (`build_body_()`).

### Bucket Strategy

| Bucket | Retention | Resolution | Purpose |
|--------|-----------|------------|---------|
| `raw` | 90 days | 5-min (as-is) | Real-time dashboards, troubleshooting |
| `hourly` | 2 years | Hourly mean/min/max/count | Trend analysis, most Grafana panels |
| `daily` | Indefinite | Daily compliance percentages | NABERS/Green Star, WELL/RESET reports |

### Downsampling & Dashboards

The downsampling Lambdas (hourly aggregation, daily compliance) and Grafana dashboard configs live in the separate `samba_web` repository. See that repo's README for CDK deployment instructions.
