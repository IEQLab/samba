# SAMBA IEQ Monitoring System

This repository contains the firmware for SAMBA v2, a low-cost indoor environmental quality (IEQ) monitor developed by the [IEQ Lab](https://www.sydney.edu.au/architecture/our-research/research-labs-and-facilities/indoor-environmental-quality-lab.html) at The University of Sydney. SAMBA runs on [ESPHome](https://esphome.io), an open-source firmware framework for ESP32 microcontrollers. The minimum supported version of ESPHome is **2026.3.0**.

### Configuration

ESPHome devices are configured using [YAML](https://yaml.org). This repository is structured so that each sensor or function has its own config file in `config/`, which are imported as [packages](https://esphome.io/components/packages/) by the main [`samba.yaml`](https://github.com/IEQLab/samba/blob/main/samba.yaml).

```
├── samba.yaml              # main config
├── config/
|   ├── adc.yaml            # analog-to-digital converter
|   ├── airspeed.yaml       # anemometers
|   ├── co2.yaml            # CO2 sensor
|   ├── diagnostics.yaml    # device diagnostics
|   ├── esp32.yaml          # ESP32 board and framework
|   ├── globals.yaml        # global variables
|   ├── homeassistant.yaml  # Home Assistant API
|   ├── illuminance.yaml    # illuminance sensor
|   ├── influx.yaml         # InfluxDB connection
|   ├── led.yaml            # status LED
|   ├── ota.yaml            # over-the-air updates
|   ├── pm25.yaml           # PM2.5 sensor
|   ├── rtc.yaml            # real-time clock
|   ├── sample.yaml         # sampling loop
|   ├── sd.yaml             # SD card logging
|   ├── spl.yaml            # sound pressure level
|   ├── substitutions.yaml  # secrets and substitutions
|   ├── tair.yaml           # air temperature and RH
|   ├── tglobe.yaml         # globe temperature
|   ├── tvoc.yaml           # TVOC and NOx sensor
|   └── wifi.yaml           # wireless networking
├── components/             # custom ESPHome components
|   ├── influxdb/           # InfluxDB v2 HTTP upload
|   ├── sd_spi_card/        # SPI SD card read/write
|   ├── senseair_i2c/       # K30/K33 CO2 over I2C
|   └── sound_level_meter/  # I2S audio DSP for SPL
├── firmware/               # compiled binaries for OTA
└── pcb/                    # hardware design files (Altium)
```

The `components/` directory contains four [external components](https://esphome.io/components/external_components.html) that extend ESPHome:

1. `sound_level_meter` — audio DSP for sound pressure level (LAeq, LA90, LA10)
2. `senseair_i2c` — I2C driver for the [K30 CO2 sensor](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module)
3. `influxdb` — HTTP upload to an InfluxDB v2 bucket
4. `sd_spi_card` — FAT32 SD card logging via SPI

### Sensors

| Parameter | Sensor | Config | Component |
|:---------:|:------:|:------:|:---------:|
| Temperature / RH | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [tair.yaml](https://github.com/IEQLab/samba/blob/main/config/tair.yaml) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |
| Globe Temperature | [NTC Thermistor](https://www.murata.com/en-us/products/productdetail?partno=NXRT15XH103FA1B040) | [tglobe.yaml](https://github.com/IEQLab/samba/blob/main/config/tglobe.yaml) | [ntc](https://esphome.io/components/sensor/ntc.html) |
| Air Speed | [Thermal Anemometer](https://moderndevice.com/products/wind-sensor) | [airspeed.yaml](https://github.com/IEQLab/samba/blob/main/config/airspeed.yaml) | [ads1115](https://esphome.io/components/sensor/ads1115.html) |
| CO2 | [CO2Meter K30](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module) | [co2.yaml](https://github.com/IEQLab/samba/blob/main/config/co2.yaml) | [senseair_i2c](https://github.com/IEQLab/samba/tree/main/components/senseair_i2c) |
| PM2.5 | [Plantower PMS5003T](https://www.plantower.com/en/products_33/74.html) | [pm25.yaml](https://github.com/IEQLab/samba/blob/main/config/pm25.yaml) | [pmsx003](https://esphome.io/components/sensor/pmsx003.html) |
| VOC / NOx Index | [Sensirion SGP40](https://sensirion.com/products/catalog/SGP40/) | [tvoc.yaml](https://github.com/IEQLab/samba/blob/main/config/tvoc.yaml) | [sgp4x](https://esphome.io/components/sensor/sgp4x.html) |
| Illuminance | [TI OPT3001](https://www.ti.com/product/OPT3001) | [illuminance.yaml](https://github.com/IEQLab/samba/blob/main/config/illuminance.yaml) | [opt3001](https://esphome.io/components/sensor/opt3001.html) |
| Sound Pressure Level | [ICS-43434 Microphone](https://invensense.tdk.com/products/ics-43434/) | [spl.yaml](https://github.com/IEQLab/samba/blob/main/config/spl.yaml) | [sound_level_meter](https://github.com/stas-sl/esphome-sound-level-meter) |

Most sensors are natively supported by ESPHome. The CO2 sensor and sound pressure level measurement use custom external components in `components/`.

### Sampling

SAMBA continuously measures environmental parameters, applies quality filters and a moving median, then publishes a summary every **5 minutes**. The routine is:

1. Each sensor measures at its own frequency (see table below).
2. Raw readings pass through [filters](https://esphome.io/components/sensor/index.html#sensor-filters) — NaN rejection, clamping, moving median — and calibration functions.
3. Every 5 minutes, a [cron task](https://esphome.io/components/time/index.html) on the RTC triggers the `sensor_sample` [script](https://esphome.io/guides/automations.html#script-component).
4. The script updates all [template sensors](https://esphome.io/components/sensor/template.html), then publishes to Home Assistant, InfluxDB, and/or the SD card. The LED flashes white with each upload.
5. Publishing is skipped during the first 2 minutes after boot (sensor warm-up).

| Measure | Frequency | Filters |
|:-------:|:---------:|:-------:|
| Air Temperature | 30s | clamp; moving median; linear calibration |
| Relative Humidity | 30s | clamp; moving median; linear calibration |
| Globe Temperature | 30s | clamp; moving median; linear calibration |
| Air Speed | 2s | clamp; moving median; multivariate calibration; clamp |
| CO2 | 30s | filter; clamp; moving median; linear calibration; clamp |
| PM2.5 | ~1s | clamp; moving median |
| VOC Index | 30s | moving median |
| NOx Index | 30s | moving median |
| Illuminance | 20s | clamp; moving median; linear calibration; clamp |
| Sound Pressure Level | 500ms | sos; moving median; quantile (LA90, LA10) |

### Data

Published measurements are sent every 5 minutes to one or more of the following backends:

**[Home Assistant](https://www.home-assistant.io)** — An open-source home automation platform. Easy to use but requires additional hardware (e.g. Raspberry Pi) and some configuration to retain raw data beyond 10 days. Best suited for projects that also collect other measurements (e.g. energy, window/door state). Communication uses the native ESPHome [API component](https://esphome.io/components/api.html).

**[InfluxDB](https://www.influxdata.com/products/influxdb-overview/)** — An open-source time series database optimised for IoT. Can be self-hosted or used via InfluxData's cloud service. Requires an active internet connection. Best suited for field deployments of multiple SAMBAs. Communication uses the custom `influxdb` component with building, level, and zone IDs as tags (set as [global variables](https://github.com/IEQLab/samba/blob/main/config/globals.yaml)). See the [InfluxDB key concepts](https://docs.influxdata.com/influxdb/v1/concepts/key_concepts/) for background.

**SD Card** — Local CSV logging via the `sd_spi_card` component. Files are named using the device MAC address and UTC timestamp. No network connection required.

### Modifying Firmware

Users are free to modify the SAMBA firmware to suit their needs. We recommend familiarity with ESPHome and microcontroller programming before doing so. To get started:

1. Define your project-specific parameters in [`secrets.yaml`](https://esphome.io/guides/yaml.html#secrets-and-the-secrets-yaml-file).
2. Modify the relevant `.yaml` files in `config/`.
3. Compile and upload via USB-C with `esphome run samba.yaml`, or wirelessly with `esphome run samba.yaml --device <IP_ADDRESS>`.

Calibration coefficients are stored as persistent [global variables](https://github.com/IEQLab/samba/blob/main/config/globals.yaml) and can be updated either by editing the lambda functions in the relevant config files or by modifying the globals directly.

The user is responsible for managing any device running modified firmware.

### OTA Updates

SAMBA devices check for firmware updates every Monday at 4 am by comparing against [`firmware/manifest.json`](https://github.com/IEQLab/samba/blob/main/firmware/manifest.json). If a new version is available, the update is applied automatically with random jitter to avoid fleet-wide simultaneous downloads. Automatic updates can be disabled via the web server toggle (see [Deployment](#deployment)).

### Deployment

SAMBA devices are shipped pre-calibrated with deployment firmware. Follow these steps to connect a new SAMBA to your network and start sampling:

1. **Power on** the SAMBA. The status LED will strobe red, green, and blue to indicate it is in setup mode.
2. **Connect to the hotspot.** Using a phone or laptop, join the `samba_connect` WiFi network and open the [captive portal](https://esphome.io/components/captive_portal.html) at [http://192.168.4.1](http://192.168.4.1).
3. **Enter WiFi credentials.** Select the target 2.4 GHz network from the list and enter the password. The SAMBA will connect and the LED will pulse blue.
4. **Open the web server.** Navigate to the SAMBA's IP address on the local network (e.g. `http://192.168.1.XXX`) in a browser. The web server displays the following configuration groups:

   | Group | What to configure |
   |:-----:|:-----------------:|
   | **Influx Tags** | Building identifier, building level, and building zone |
   | **Device Configuration** | Toggles for InfluxDB uploads, SD card logging, and automatic updates |
   | **Calibration Coefficients** | Pre-set during calibration — no changes needed |
   | **Measurements** | Live sensor readings for verification |
   | **Firmware** | Buttons to flash production or test firmware |

5. **Set location tags.** Enter the building name, level, and zone for this deployment. These are used as InfluxDB tags for all uploaded measurements. We recommend using lower case and avoiding special characters e.g. `building_name`.
6. **Adjust settings.** Enable or disable InfluxDB uploads, SD card logging, and automatic firmware updates as needed.
7. **Flash production firmware.** Click **"Flash production firmware"** to download and install the latest firmware from this repository. The SAMBA will reboot and begin sampling automatically. The LED will blink green during the warm-up period and then turn off once it enters the normal sampling routine.

The setup and calibration process is managed separately. If you need to calibrate or re-flash deployment firmware on a SAMBA, please reach out — see [Project Maintenance](#project-maintenance) below.

### Project Maintenance

This is an active project to build an open research platform for healthy, high-performance buildings. If you're interested in using SAMBA in your project or contributing to its development, start a [Github discussion](https://github.com/IEQLab/samba/discussions) or email Tom.
