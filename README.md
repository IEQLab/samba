# SAMBA IEQ Monitoring System

This repository contains all the firmware files for SAMBA v2. The SAMBA system was developed by the [IEQ Lab](https://www.sydney.edu.au/architecture/our-research/research-labs-and-facilities/indoor-environmental-quality-lab.html) at The University of Sydney. The latest version runs on [ESPHome](https://esphome.io), which is an active, open-source system designed for microcontrollers to do home automation tasks.

### Configuration

ESPHome devices are configured using [YAML](https://yaml.org). YAML is commonly used for configuration files as it is human-readable and clearly structured. Settings are expressed as key-value pairs. There are many [YAML guides](https://www.cloudbees.com/blog/yaml-tutorial-everything-you-need-get-started) online if you are unfamiliar with the syntax.

This repository is structured to allow easy modification of the configuration files using YAML. ESPHome expects config files in `/esphome` and additional components in `/esphome/components`. The directory structure is based on subdirectories that group individual config files based on their type. For example, the configuration files for all sensor components are in `/esphome/sensors`. `/esphome/base` has config files for the board, wifi, over-the-air updates, real-time clock, analog to digital converter, LED, and home assistant. And `/esphome/measure` includes the script that handles the measurement loop and the config file for the InfluxDB upload.

```         
├── esphome
|   ├── base        # esp32, wifi, OTA, RTC, ADC, LED, Home Assistant
|   ├── components  # i2s, InfluxDB, sound level meter
|   ├── measure     # sampling loop, data upload
|   ├── sensors     # ta/rh, ntc, airspeed, pm2.5, co2, tvoc/nox, lux, spl
|   └── ...
```

The configuration of these components has been setup for the reliable operation of SAMBA as per the IEQ Lab specification. However, it is possible for someone to modify the way it works by editing these files. An example configuration for the temperature and humidity sensor is as follows:

```
sensor:
  - platform: sht4x
    temperature:
      name: "Temperature"
    humidity:
      name: "Relative Humidity"
```

This is a basic configuration that will return temperature and relative humidity. The full set of configuration options are given on the [SHT4X component page](https://esphome.io/components/sensor/sht4x.html) of ESPHome. Comments throughout the .yaml files point the interested user to the relevant documentation. Note that modifications are the responsibility of the user and are not supported by the IEQ Lab.

The `/esphome/components` subdirectory is where ESPHome expects to find [external components](https://esphome.io/components/external_components.html) that provide additional functionality beyond what is offered natively in ESPHome. We are using three external components in SAMBA: `i2s` to sample audio with the microphone, `influxDB` for uploading samples to an InfluxDB bucket, and `sound_level_meter` for calculating SPL (eq, dBA, dBC).

### Sensors

SAMBA is equipped with sensors that are configured using .yaml files in `/esphome/sensors`. Most of the sensors are natively supported by ESPHome through what are known as 'components'. The chosen sensors are commonly used by the hobbyist and home automation communities for their reliability and support. The only sensor not directly supported by ESPHome is the microphone, which uses the `i2s` and `sound_level_meter` external components.

|     Parameter     |                              Sensor                              |                                         Config                                         |                        Component                         |
|:--------------------:|:---------------:|:---------------:|:---------------:|
|  Temperature/RH  | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [thermal.yaml](https://github.com/IEQLab/samba/blob/b07876be9d153c4315995ed3d519412e2f8a302a/esphome/sensors/thermal.yaml#L9-L23) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |
| Globe Temperature | [NTC Thermistor](https://www.murata.com/en-us/products/productdetail?partno=NXRT15XH103FA1B040) | [thermal.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/thermal.yaml) | [ntc](https://esphome.io/components/sensor/ntc.html) |
| Air Speed | [Thermal Anemometer](https://moderndevice.com/products/wind-sensor) | [thermal.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/thermal.yaml) | [ads1115](https://esphome.io/components/sensor/ads1115.html) |
| CO2 | [CO2Meter K30](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [TBD](https://github.com/esphome/feature-requests/issues/1587) |
| PM2.5 | [Plantower PMS5003](https://www.plantower.com/en/products_33/74.html) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [pmsx003](https://esphome.io/components/sensor/pmsx003.html) |
| VOC/NOx Index | [Sensirion SGP40](https://sensirion.com/products/catalog/SGP40/) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [sgp4x](https://esphome.io/components/sensor/sgp4x.html) |
| Illuminance | [OSRAM TSL2591](https://sensirion.com/products/catalog/SGP40/) | [light.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/light.yaml) | [tsl2591](https://esphome.io/components/sensor/tsl2591.html) |
| Sound Pressure Level | [ICS-43434 Microphone](https://invensense.tdk.com/products/ics-43434/) | [sound.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/light.yaml) | [sound_level_meter](https://github.com/stas-sl/esphome-sound-level-meter) |

### Sampling

A key part of the SAMBA configuration is the sampling routine. SAMBA is configured to constantly measure environmental parameters, periodically summarise those measurements using the median with a moving window, and then upload them at a set interval. The sampling routine is as follows:

1.  Each sensor is set to measure at a given frequency. This ranges from 500ms (for SPL) to 60s for VOC/NOx Index; see table below for summary.
2.  Simple quality assurance is done on the measurements. Some sensors have a [filter](https://esphome.io/components/sensor/index.html#filter-out) to remove obvious outliers e.g. temperatures below -10°C and above 60°C. Some sensors have a [linear calibration](https://esphome.io/components/sensor/index.html#calibrate-linear) e.g. converting voltage to airspeed. All sensors use a [simple moving median](https://esphome.io/components/sensor/index.html#median) that updates the sensor value every 30s.
3.  The sample loop runs every 5-minutes using a [cron task on the RTC](https://esphome.io/components/time/index.html). The loop is a [script component](https://esphome.io/guides/automations.html#script-component) that executes a set of steps; details are given in [loop.yaml](https://github.com/IEQLab/samba/blob/main/esphome/measure/loop.yaml) in `/esphome/measure`. 
4. The script first updates the [template sensors](https://esphome.io/components/sensor/template.html) to retrieve the latest measurement from the sensor (after the simple moving median), and then publishes those data to Home Assistant or InfluxDB.

The following table summarises the sampling and filters (ordered sequentially) used in the default SAMBA configuration. Again, users are free to modify this but no support will be offered by the IEQ Lab.

| Measure | Frequency | Filters |
|:-------:|:---------:|:-------:|
| Air Temperature | 30s | outliers; moving median |
| Relative Humidity | 30s | outliers; moving median |
| Globe Temperature | 30s | linear calibration; outliers; moving median |
| Air Speed | 1s | linear calibration; outliers; moving median |
| CO2 | 30s | outliers; moving median |
| PM2.5 | 1s | outliers; moving median |
| VOC Index | 30s | moving median |
| NOx Index | 30s | moving median |
| Illuminance | 10s | outliers; moving median |
| Sound Pressure Level | 500ms | sos; moving median |

### Data

The published measurements from SAMBA get sent every 5-minutes to a Home Assistant instance and/or an InfluxDB bucket. 

[Home Assistant](https://www.home-assistant.io) is an open-source home automation platform that integrates thousands of consumer devices in a no-code environment. The advantage of Home Assistant is it is easy to use (especially for non-technical types). The disadvantage is that it requires another piece of hardware (e.g. Raspberry Pi) and it requires some tinkering to store raw data longer than 10 days. It is most beneficial if the research project involves other kinds of measurements e.g. energy monitoring or window/door openings. Communication between the SAMBA and Home Assistant is done using the native [API Component](https://esphome.io/components/api.html) in ESPHome.

[InfluxDB](https://www.influxdata.com/products/influxdb-overview/) is an open-source time series database. It has high-speed read and write that is optimised for real-time data in IoT applications. It can be deployed on most server environments for free, and [InfluxData](https://www.influxdata.com) offer a hosted service that has a free and paid tiers (based on usage). The advantage is that it removes additional hardware layers. The disadvantage is that it requires the SAMBA device to have an active internet connection. It is most beneficial when the research project is deploying SAMBAs only e.g. a field study in office buildings. Communication between the SAMBA and InfluxDB is done using the external component in `esphome/components/influxdb`.

If you want to modify the InfluxDB configuration then we recommend familiarising yourself with [key concepts of InfluxDB](https://docs.influxdata.com/influxdb/v1/concepts/key_concepts/) first, like measurement, tags, and fields. No support is given for InfluxDB buckets other than those maintained by the IEQ Lab.

### Setup

It is possible to modify the configuration of SAMBA using the [ESPHome Command Line Interface](https://esphome.io/guides/cli.html). The `samba.yaml` script in the root directory will source all the relevant files. Steps to do so would look something like:

1.  [Install ESPHome CLI](https://esphome.io/guides/getting_started_command_line.html).
2.  Clone the [SAMBA Github Repository](https://github.com/IEQLab/samba/tree/main) to your local device.
3.  Make whatever modifications to the relevant .yaml files in `esphome/...`.
4.  Connect your laptop to SAMBA using a USB-C cable.
4.  Open a terminal window and `chdir` to the working folder where the SAMBA Github repo was cloned.
5.  Enter the following command to compile and upload the binary file to the SAMBA: `esphome run samba.yaml`. Note that you will need to select the serial device before uploading.

You can also do [over-the-air firmware updates](https://esphome.io/components/ota.html) if you are on the same WLAN as the SAMBA device.
