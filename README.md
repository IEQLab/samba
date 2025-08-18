# SAMBA IEQ Monitoring System

This repository contains all the firmware files for SAMBA v2. The SAMBA system was developed by the [IEQ Lab](https://www.sydney.edu.au/architecture/our-research/research-labs-and-facilities/indoor-environmental-quality-lab.html) at The University of Sydney. The latest version runs on [ESPHome](https://esphome.io), which is an active, open-source system designed for microcontrollers to do home automation tasks.

### ⚙️ Configuration

ESPHome devices are configured using [YAML](https://yaml.org). YAML is commonly used for configuration files as it is human-readable and clearly structured. Settings are expressed as key-value pairs. There are many [YAML guides](https://www.cloudbees.com/blog/yaml-tutorial-everything-you-need-get-started) online if you are unfamiliar with the syntax.

This repository is structured to allow modification of the configuration files using YAML. ESPHome config files are in `config/`. Individual config files typically serve a single component or function as summarised below.

```         
├── config
|   ├── adc.yaml            # analog-to-digital converter
|   ├── airspeed.yaml       # anemometers
|   ├── co2.yaml            # CO2 sensor
|   ├── diagnostics.yaml    # device diagnostics
|   ├── esp32.yaml          # esp32 configuration
|   ├── globals.yaml        # global variables
|   ├── homeassistant.yaml  # Home Assistant API
|   ├── illuminance.yaml    # illuminance sensor
|   ├── influx.yaml         # InfluxDB connection
|   ├── led.yaml            # status LED
|   ├── ota.yaml            # over-the-air updates
|   ├── pm25.yaml           # PM2.5 sensor
|   ├── rtc.yaml            # clocks
|   ├── sample.yaml         # sampling loop
|   ├── spl.yaml            # sound pressure level sensor
|   ├── substitutions.yaml  # substitutions
|   ├── tair.yaml           # air temperature and RH sensor
|   ├── tglobe.yaml         # globe temperature sensor
|   ├── tvoc.yaml           # TVOC and NOx sensor
|   ├── wifi.yaml           # wireless networking
|   └── ...
```

The configuration of these components has been setup for the reliable operation of SAMBA as per the requirements of the IEQ Lab. However, it is possible for someone to modify the way SAMBA works by editing these files. An example configuration for the temperature and humidity sensor is as follows:

```
sensor:
  - platform: sht4x
    temperature:
      name: "Temperature"
    humidity:
      name: "Relative Humidity"
```

This is a basic configuration that will return temperature and relative humidity. The full set of configuration options are given on the [SHT4X component page](https://esphome.io/components/sensor/sht4x.html) of ESPHome. Comments throughout the .yaml files point the interested user to the relevant documentation. Note that modifications are the responsibility of the user and are not supported by the IEQ Lab.

The `components/` subdirectory is where ESPHome expects to find [external components](https://esphome.io/components/external_components.html) that provide additional functionality beyond what is offered natively in ESPHome. We are using four external components in SAMBA: 

1.  `i2s` to sample audio with the microphone.
2.  `sound_level_meter` for calculating SPL (eq, dBA, dBC).
3.  `senseair_i2c` to communicate with [K30 CO2 sensor](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module). 
4.  `influxdb_writer` for uploading samples to an InfluxDB bucket.

The hope is to have these external components merged into esphome at some point so the broader community can use them.

### 📏 Sensors

SAMBA is equipped with sensors that are configured using .yaml files in `config/`. Most of the sensors are natively supported by ESPHome through what are known as 'components'. The chosen sensors are commonly used by the hobbyist and home automation communities for their reliability and support. The only sensor not directly supported by ESPHome is the CO2 sensor and sound pressure level, which uses the `i2s` and `sound_level_meter` external components.

|     Parameter     |                              Sensor                              |                                         Config                                         |                        Component                         |
|:--------------------:|:---------------:|:---------------:|:---------------:|
|  Temperature/RH  | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [tair.yaml](https://github.com/IEQLab/samba/blob/main/config/tair.yaml) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |
| Globe Temperature | [NTC Thermistor](https://www.murata.com/en-us/products/productdetail?partno=NXRT15XH103FA1B040) | [tglobe.yaml](https://github.com/IEQLab/samba/blob/main/config/tglobe.yaml) | [ntc](https://esphome.io/components/sensor/ntc.html) |
| Air Speed | [Thermal Anemometer](https://moderndevice.com/products/wind-sensor) | [airspeed.yaml](https://github.com/IEQLab/samba/blob/main/config/airspeed.yaml) | [ads1115](https://esphome.io/components/sensor/ads1115.html) |
| CO2 | [CO2Meter K30](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module) | [co2.yaml](https://github.com/IEQLab/samba/blob/main/config/co2.yaml) | [senseair_i2c](https://github.com/IEQLab/samba/tree/main/components/senseair_i2c) |
| PM2.5 | [Plantower PMS5003](https://www.plantower.com/en/products_33/74.html) | [pm25.yaml](https://github.com/IEQLab/samba/blob/main/config/pm25.yaml) | [pmsx003](https://esphome.io/components/sensor/pmsx003.html) |
| VOC/NOx Index | [Sensirion SGP40](https://sensirion.com/products/catalog/SGP40/) | [tvoc.yaml](https://github.com/IEQLab/samba/blob/main/config/tvoc.yaml) | [sgp4x](https://esphome.io/components/sensor/sgp4x.html) |
| Illuminance | [TI OPT3001](https://www.ti.com/product/OPT3001) | [illuminance.yaml](https://github.com/IEQLab/samba/blob/main/config/illuminance.yaml) | [opt3001](https://esphome.io/components/sensor/opt3001.html) |
| Sound Pressure Level | [ICS-43434 Microphone](https://invensense.tdk.com/products/ics-43434/) | [spl.yaml](https://github.com/IEQLab/samba/blob/main/config/spl.yaml) | [sound_level_meter](https://github.com/stas-sl/esphome-sound-level-meter) |

### 🔄 Sampling

A key part of the SAMBA configuration is the sampling routine. SAMBA is configured to constantly measure environmental parameters, periodically summarise those measurements using the median with a moving window, and then upload them at a set interval. The sampling routine is as follows:

1.  Each sensor is set to measure at a given frequency. This ranges from 500ms (for SPL) to 60s for VOC/NOx Index; see table below for summary.
2.  Simple quality assurance is done on the measurements. Sensors have a [filter](https://esphome.io/components/sensor/index.html#clamp) to remove obvious outliers e.g. temperatures below -10°C and above 60°C. Some have sensors have a linear calibration (implemented as a [lambda function](https://esphome.io/cookbook/lambda_magic.html)) e.g. converting voltage to airspeed. All sensors use a [simple moving median](https://esphome.io/components/sensor/index.html#median) that updates the sensor value every 30s.
3.  The sample loop is triggered every 5-minutes using a [cron task on the RTC](https://esphome.io/components/time/index.html). The loop is a [script component](https://esphome.io/guides/automations.html#script-component) that executes a set of steps; details are given in [sample.yaml](https://github.com/IEQLab/samba/blob/main/config/sample.yaml) in `config/`. 
4. The script first updates the [template sensors](https://esphome.io/components/sensor/template.html) to retrieve the latest measurement from the sensor (after the filters and moving median), and then publishes those data to Home Assistant or InfluxDB. The LED will flash with each upload.

The following table summarises the sampling and filters (ordered sequentially) used in the default SAMBA configuration. Again, users are free to modify this but no support will be offered by the IEQ Lab.

| Measure | Frequency | Filters |
|:-------:|:---------:|:-------:|
| Air Temperature | 30s | clamp; moving median; linear calibration |
| Relative Humidity | 30s | clamp; moving median; linear calibration |
| Globe Temperature | 30s | clamp; moving median; linear calibration |
| Air Speed | 1s | clamp; moving median; multivariate calibration; clamp |
| CO2 | 30s | filter; clamp; moving median; linear calibration; clamp |
| PM2.5 | 1s | clamp; moving median |
| VOC Index | 30s | moving median |
| NOx Index | 30s | moving median |
| Illuminance | 10s | clamp; moving median; linear calibration; clamp  |
| Sound Pressure Level | 500ms | sos; moving median |

### 📈 Data

The published measurements from SAMBA get sent every 5-minutes to a Home Assistant instance and/or an InfluxDB bucket. 

[Home Assistant](https://www.home-assistant.io) is an open-source home automation platform that integrates thousands of consumer devices in a no-code environment. The advantage of Home Assistant is it is easy to use (especially for non-technical types). The disadvantage is that it requires another piece of hardware (e.g. Raspberry Pi) and it requires some tinkering to store raw data longer than 10 days. It is most beneficial if the research project involves other kinds of measurements e.g. energy monitoring or window/door openings. Communication between the SAMBA and Home Assistant is done using the native [API Component](https://esphome.io/components/api.html) in ESPHome.

[InfluxDB](https://www.influxdata.com/products/influxdb-overview/) is an open-source time series database. It has high-speed read and write that is optimised for real-time data in IoT applications. It can be deployed on most server environments for free, and [InfluxData](https://www.influxdata.com) offer a hosted service that has a free and paid tiers (based on usage). The advantage is that it removes additional hardware layers. The disadvantage is that it requires the SAMBA device to have an active internet connection. It is most beneficial when the research project is deploying SAMBAs only e.g. a field study in office buildings. Communication between the SAMBA and InfluxDB is done using the external component in `components/influxdb_writer`.

If you want to modify the InfluxDB configuration then we recommend familiarising yourself with [key concepts of InfluxDB](https://docs.influxdata.com/influxdb/v1/concepts/key_concepts/) first, like measurement, tags, and fields. No support is given for InfluxDB buckets other than those maintained by the IEQ Lab.

### 🚀️ User Guide ###

The following section details how every SAMBA device is configured by the IEQ Lab. To begin, make sure the [ESPHome Command Line Interface](https://esphome.io/guides/cli.html) is installed. Instructions can be found on the [ESPHome website](https://esphome.io/guides/installing_esphome).

SAMBAs are initially flashed with [`setup.yaml`](https://github.com/IEQLab/samba/blob/main/setup.yaml) in the root directory, a basic configuration that loads device-specific parameters (e.g. calibration coefficients), creates a wireless hotspot to configure WiFi networks, and then downloads the latest SAMBA firmware from Github. The latest SAMBA firmware is compiled from [`samba.yaml`](https://github.com/IEQLab/samba/blob/main/samba.yaml) in the root directory, which will source all the relevant configuration in `config/`.

#### 🌱 Initial Setup ####

Access to the ESP32 is through the USB-C port on SAMBAs main PCB. There is a single bolt that holds the SAMBA housing together - use a 4mm hex key to undo the bolt on the bottom of the device. Slide the bolt out from the bottom to remove the housing and reveal the PCB. The USB-C port is located on the bottom right of the PCB. Note: this will not provide power to the SAMBA; power must be provided through the DC barrel jack. Once your device is connected and the SAMBA is powered on, follow these steps to flash the setup firmware:

0. [Optional] Erase the ESP32 flash memory from earlier deployments: `esptool.py --chip esp32 erase_flash`.
1.  Clone the [SAMBA Github Repository](https://github.com/IEQLab/samba/tree/main) to your local device and open that directory.
2.  Open a terminal window and `cd` to the working folder where the SAMBA Github repo was cloned.
3.  The initial setup script is `samba_setup.yaml`. It contains placeholder calibration coefficients - these can be changed later.
4.  Enter the following command to compile and upload the binary file to the SAMBA: `esphome run samba_setup.yaml`. Note that you will need to select the serial device before uploading; alternatively, specify the serial connection e.g. `esphome run setup.yaml --device /dev/cu.usbserial-10`.

It should take about 30 seconds to flash the firmware. The Led should flash green and blue to indicate it is on but not connected to WiFi. If the SAMBA is being relocated, disconnect the USB-C cable once it is finished flashing, unplug the power, put the housing back on, and tighten the hex bolt. Place the SAMBA in its new location, power it on, and follow the below steps to configure the WiFi:

1.  Use another device (e.g. smartphone, laptp) to join the `samba_connect` ad-hoc WiFi and open the [captive portal](https://esphome.io/components/captive_portal.html) by entering [http://192.168.4.1/](http://192.168.4.1/) into your browser.
2.  Select the 2.4GHz network to join from the list and enter in the password.
3.  The SAMBA will connect to the network and then launch a web server to display the configuration settings and terminal window. This can be accessed through a browser at the IP address of the SAMBA e.g. `192.168.1.50`. 
4.  Enter the location (building name, level/room number, and zone name) and the calibration coefficients. CO2, illuminance, air temperature, relative humidity, and globe temperature are linear calibrations (e.g. y = mx + b); the two air speed sensors are power regressions (e.g. y = a * x^b).
3.  Once the configuration is complete, click the 'Deploy SAMBA' button to attempt to download and flash the latest firmware stored in [`firmware/`](https://github.com/IEQLab/samba/tree/main/firmware) on the IEQ Lab Github. Once that is done, the SAMBA will reboot and start sampling automatically. The status LED should blink slower to indicate it is warming up; this will stop once it enters the sampling routine.

#### 🎯 Compiling Base Firmware [IEQ Lab] ####

The IEQ Lab will actively maintain the SAMBA firmware and publish updates to the [Github repository](https://github.com/IEQLab/samba/tree/main). This might involve performance improvements or additional features. In this case, the IEQ Lab will push new binaries to `firmware/` which can then be flashed remotely using [OTA updates](https://esphome.io/components/ota/http_request.html). For Lab staff wishing to update the firmware, follow these steps:

1.  [Required] Speak to Tom before doing anything 🤠
2.  Make the agreed modifications to the relevant .yaml files in `config/...` and test EXTENSIVELY.
3.  Bump the project version in [`esp32.yaml`](https://github.com/IEQLab/samba/blob/ebebc4b091f836f893ec4236af8086405198ec6a/config/esp32.yaml#L16)
4.  Once the new firmware is confirmed stable, generate the compiled bin: `esphome compile samba.yaml`
5.  Move the compiled ota firmware to the firmware directory: `cp .esphome/build/samba/.pioenvs/samba/firmware.ota.bin firmware/samba_v2.XX.XX.bin`
6.  Generate a new md5 hash and print it to terminal: `md5 firmware/samba_v2.XX.XX.bin`
7.  Update the [`manifest.json`](https://github.com/IEQLab/samba/blob/main/manifest.json) file to include the new bin path and md5 hash.
7.  Push the new .bin and updated manifest.json to the SAMBA Github repository.

Currently, SAMBAs only check for new firmware during their initial setup. Future firmware will implement routine updates e.g. check once per week. As such, it's extremely important that firmware are extensively tested otherwise they could (worst case scenario) brick the entire SAMBA fleet.

#### 🆕 Modifying Firmware ####

Users are free to modify the SAMBA firmware to suit their needs. However, we strongly discourage people from doing this if they are not familiar with basic programming of microcontrollers and/or ESPHome. If you're feeling brave, follow these steps to customise the SAMBA firmware:

1.  Define your project-specific parameters in the [secrets.yaml](https://esphome.io/guides/yaml.html#secrets-and-the-secrets-yaml-file).
2.  Make whatever modifications to the relevant .yaml files in `config/...`.
3.  [Optional] There are two ways to update any calibration coefficients:
    - Change the relevant lambda function in the template sensor to the new value. For example, changing the CO2 regression slope would mean modifying [this line](https://github.com/IEQLab/samba/blob/ebebc4b091f836f893ec4236af8086405198ec6a/config/co2.yaml#L37) so that `id(calibration_co2_m)` becomes the new coefficient value.
    - Modify the global variable that stores the calibration coefficient. This is more robust but requires a bit more work to get right - speak to Tom if needed.
4.  Compile and upload the firmware to SAMBA via USB-C with `esphome run samba.yaml` or wirelessly (if in the same WLAN) with the SAMBA IP address `esphome run samba.yaml --device 192.168.1.XXX`.

The user is responsible for managing the device if the firmware is modified.
