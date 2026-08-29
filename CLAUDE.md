# CLAUDE.md — SAMBA Project Guide

## Project Overview

SAMBA is an ESPHome-based firmware for indoor environmental quality (IEQ) monitoring, developed by the IEQ Lab at The University of Sydney. It runs on an ESP32 WROOM-32E (16MB flash) using the ESP-IDF framework (not Arduino). The device measures temperature, humidity, globe temperature, air speed, CO2, PM2.5, VOC/NOx, illuminance, and sound pressure level, logging data to InfluxDB, Home Assistant, and SD card.

**Current version:** Check `samba.yaml` for the latest version.
**Min ESPHome version:** 2026.8.1

## Repository Structure

```
samba.yaml              # Main config: ESPHome settings, logger, packages, external components
config/                 # Modular YAML configs (one per function/sensor)
  esp32.yaml            # Board, framework (ESP-IDF), I2C, UART, SPI, sdkconfig
  substitutions.yaml    # Secrets placeholders (WiFi, OTA, InfluxDB credentials)
  globals.yaml          # Persistent calibration coefficients and enable flags
  calibration.yaml      # Calibration coefficients as native-API number entities
  tags.yaml             # InfluxDB building/level/zone tags as native-API text entities
  sample.yaml           # 5-minute sampling loop (sensor update + publish + SD append)
  rtc.yaml              # DS1307 RTC, SNTP sync, sample trigger, firmware check
  sd.yaml               # SD card mount/write via sd_spi_card component
  influx.yaml           # InfluxDB v2 upload config + token provisioning over the native API
  homeassistant.yaml    # Native API endpoint (Noise encryption, key provisioned at runtime)
  wifi.yaml             # WiFi and captive portal
  ota.yaml              # HTTP OTA updates + esphome OTA password provisioning
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
  i2c_recovery/         # Runtime I2C bus reset (clocks out a wedged target)
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

A failed sensor costs only its own measurands: the raw read publishes NaN, so the field is
absent in InfluxDB, `nan` on SD and unknown in Home Assistant, and every other sensor keeps
reporting. A restart is the last resort, never the first response — the watchdogs used to fire
faster than the 5min sample, so one dead sensor took the whole unit off the air.

All three I2C sensors keep an hour-scale EWMA duty cycle (`k30_unhealthy`, `ads_unhealthy`,
`sgp_unhealthy`), each reading directly as **seconds failed per hour** at steady state
(`3571 x duty`). Divide by `35.71` for a percentage. Only the K30 and ADS1115 escalate to a
restart, and both also require `sys_uptime > 3600`, which self-rate-limits to one attempt an hour.

- **CO2 (K30):** re-initialise on the first failure, then every 10th (5min) while it stays
  down. Restart only at `k30_error_count >= 4` **and** `k30_unhealthy > 2400` (67% of the hour),
  so a flaky sensor rides it out and only a near-dead one reboots.
- **VOC/NOx (SGP4x):** **never restarts the device** — see `config/tvoc.yaml`. `sgp4x` only
  `mark_failed()`s in `setup()` and the self test; every runtime read failure is
  `status_set_warning()` and the component clears it itself on the next good read. A reboot would
  also discard the `learning_time_offset_hours: 720` gas baseline. Error counting skips the first
  100s warmup. `sgp_unhealthy` is diagnostic only.
- **ADS1115:** soft recovery first — `i2c_recovery_a.reset_bus()` clocks out a stuck target at
  the warning threshold (90 ticks / 3min), rate-limited to 30s apart and capped at three attempts
  per episode. Restart needs all three of `ads_error_count >= 150` (5min), `ads_unhealthy > 1250`
  (35% of the hour) and uptime over an hour. The counter measures per-channel **staleness** of the
  last successful read, not `isnan()` — a failed ADS1115 read leaves `.state` at its last good value.
- **System:** safe mode on boot crash, periodic SD card presence check.

The status LED is driven from one arbiter in `config/led.yaml` polling these counters every 10s,
so an alarm survives the 5min sample heartbeat. Colour identifies the subsystem — **amber**
ADS1115, **blue** K30, **magenta** SGP4x — and pulsing versus solid identifies severity. Cyan and
purple are retired. See the Status LED table in README.md.

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

- ESPHome 2026.8.1+ installed
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

Note that `!secret` only keeps a value out of the YAML: ESPHome bakes substitutions into the
generated C++ and therefore into the binary, and `firmware/*.bin` is committed to this public
repo. No credential has to be there any more: the WiFi password never was (captive portal),
and the three below are provisioned at runtime — see next section.

### Credential provisioning

Three credentials are written **at runtime** over the native API instead of compiled in. Each
is **one per building**, held by the provisioning client `samba_app` (`samba deploy`,
`samba buildings`), and kept on the device in NVS, which OTA never rewrites. Order matters:
`samba deploy` sets the API key first so everything after it travels encrypted.

1. **Native API key** (`config/homeassistant.yaml`, `encryption: {}`). ESPHome's own
   mechanism: with no `key:` in YAML the device boots unkeyed, accepts a Noise handshake with
   the all-zeros PSK (or plaintext, removed upstream after 2027.2.0), and lets the client set
   a key with `NoiseEncryptionSetKeyRequest` (`aioesphomeapi.noise_encryption_set_key`). The
   key is saved to NVS, clients are disconnected, and the device is Noise-only from then on.
   A new key sent over an authenticated connection replaces it; an empty one clears it. There
   is no compiled-in fallback and nothing to publish: `dump_config` reports `Noise encryption:
   YES/NO`. This is the key Home Assistant or any local client uses.
2. **InfluxDB token** (`config/influx.yaml`). `influx_token` (`config/globals.yaml`,
   `max_restore_data_length: 88`) is set by the `influx_set_token` api action, which hands it
   to the component (`InfluxDB::set_token_override`) and flushes NVS. A runtime token takes
   precedence over `influx_token` from `secrets.yaml`, which is the **fallback**: leave it set
   while units are still being provisioned, then set it to `""` to ship a build with no
   token in it. An unprovisioned unit on such a build skips uploads and reports `no token`.
   The `InfluxDB Token` text sensor carries only a fingerprint (`esphome::fnv1_hash`, 8 hex
   digits, `unset` when empty) and `InfluxDB Status` the outcome of the last upload (`HTTP
   204`, `HTTP 401`, `connection failed`, `no token`), published by the component
   (`status_text_sensor`). The action is followed by a real write (a `device_status` line
   with only the uptime) so the status reflects the new token immediately. Never log the
   token; `dump_config` says only provisioned / compiled-in / none.
3. **`esphome` OTA password** (`config/ota.yaml`). Same shape: `ota_password` global
   (`max_restore_data_length: 64`), `ota_set_password` action calling
   `set_auth_password()` on `ota_esphome`, `on_boot` (priority 600) re-applies it, `OTA
   Password` text sensor is the fingerprint. `ota_password` in `secrets.yaml` is the
   fallback exactly as for the token; keep `password:` present in YAML even as `""`, which
   is what makes ESPHome compile the auth path. **Safe mode never reaches `on_boot`** (the
   trigger registers after the early return), so a crash-looping unit serves OTA with the
   compiled-in password only — and with none once the fallback is `""`. This is accepted:
   it exists only on a unit that is already broken, it is the sole recovery route there,
   and the alternative is the shared secret this removes.

Nothing publishes a credential back. A text entity was rejected for this on purpose: a text
entity's state goes to every connected API client.

Field units without LAN access stay on the fallbacks (and unkeyed) until a tech is on site;
none of the three transitions changes their behaviour until then. Revoke the old InfluxDB
token only once every unit reports a fingerprint — it is in every published `.bin` in git
history, so removing it from the next build is not what closes the hole.

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
