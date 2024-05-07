# SAMBA IEQ Monitoring System

This repository contains all the firmware files for SAMBA v2. The SAMBA system was developed by the [IEQ Lab](https://www.sydney.edu.au/architecture/our-research/research-labs-and-facilities/indoor-environmental-quality-lab.html) at The University of Sydney. The latest version runs on [ESPHome](https://esphome.io), which is an actively-maintained open source system designed to control microcontrollers for home automation tasks.

### Configuration

ESPHome devices are configured using [YAML](https://yaml.org). YAML is commonly used for configuration files as it is human-readable. Settings are expressed as key-value pairs. There are many [YAML guides](https://www.cloudbees.com/blog/yaml-tutorial-everything-you-need-get-started) that you can follow if you are unfamiliar.

The repository is structured to allow easy modification of the configuration files using YAML. ESPHome expects config files in the `esphome` directory and additional components in the `esphome/components` subdirectory. The repository is structured accordingly.

```         
├── esphome
|   ├── base        # esp32, wifi, OTA, RTC, ADC, LED, Home Assistant
|   ├── components  # i2s, InfluxDB, sound level meter
|   ├── measure     # sampling loop, data upload
|   ├── sensors     # ta/rh, ntc, airspeed, pm2.5, co2, tvoc/nox, lux, spl
|   └── ...
```

The directory structure is based on subdirectories that group individual config files based on their type. For example, the configuration files for all sensor components are in the `sensors` subdirectory. `base` has config files for the board, wifi, over-the-air updates, real-time clock, analog to digital converter, LED, and home assistant. And `measure` includes the script that handles the measurement loop and the config file for the InfluxDB upload.

The configuration of these components has been setup for the reliable operation of SAMBA as per the IEQ Lab specification. However, it is possible for someone to modify the way it works by editing these files. An example configuration for the temperature and humidity sensor is as follows:

.. code-block:: yaml

    sensor:
      - platform: sht4x
        temperature:
          name: "Temperature"
        humidity:
          name: "Relative Humidity"

This is a basic configuration that will report temperature and relative humidity. The full set of configuration options are given on the [SHT4X device page](https://esphome.io/components/sensor/sht4x.html) of ESPHome. Comments throughout the .yaml files point the interested user to the relevant documentation. Note that modifications are the responsibility of the user and are not supported by the IEQ Lab.

The `components` subdirectory is where ESPHome expects to find [external components](https://esphome.io/components/external_components.html) that additional functionality beyond what is offered natively in ESPHome. We are using three external components in SAMBA: `i2s` to sample from the microphone, `influxDB` for uploading samples to the InfluxDB bucket, and `sound_level_meter` for calculating SPL (eq, dBA, dBC).

### Sensors

The following sensors are installed on SAMBA and configured using .yaml files in the `sensors` subdirectory. Most of the sensors are natively supported by ESPHome through what are known as 'components'. The sensors are commonly used in the hobbyist and home automation communities. The only sensor not directly supported by ESPHome is the microphone, which uses the `i2s` and `sound_level_meter` external components.

|     Parameter     |                              Sensor                              |                                         Config                                         |                        Component                         |
|:--------------------:|:---------------:|:---------------:|:---------------:|
|  Temperature/RH  | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [thermal.yaml](https://github.com/IEQLab/samba/blob/b07876be9d153c4315995ed3d519412e2f8a302a/esphome/sensors/thermal.yaml#L9-L23) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |
| Globe Temperature | [NTC Thermistor](https://www.murata.com/en-us/products/productdetail?partno=NXRT15XH103FA1B040) | [thermal.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/thermal.yaml) | [ntc](https://esphome.io/components/sensor/ntc.html) |
| Air Speed | [Thermal Anemometer](https://moderndevice.com/products/wind-sensor) | [thermal.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/thermal.yaml) | [ads1115](https://esphome.io/components/sensor/ads1115.html) |
| CO2 | [CO2Meter K30](https://www.co2meter.com/en-au/products/k-30-co2-sensor-module) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [TBD](https://github.com/esphome/feature-requests/issues/1587) |
| PM2.5 | [Plantower PMS5003](https://www.plantower.com/en/products_33/74.html) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [pmsx003](https://esphome.io/components/sensor/pmsx003.html) |
| VOC/NOx Index | [Sensirion SGP40](https://sensirion.com/products/catalog/SGP40/) | [iaq.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/iaq.yaml) | [sgp4x](https://esphome.io/components/sensor/sgp4x.html) |
| Illuminance | [OSRAM TSL2591](https://sensirion.com/products/catalog/SGP40/) | [light.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/light.yaml) | [tsl2591](https://esphome.io/components/sensor/tsl2591.html) |
| Sound Pressure Level | [ICS-43434 MEMS Micrphone](https://invensense.tdk.com/products/ics-43434/) | [sound.yaml](https://github.com/IEQLab/samba/blob/main/esphome/sensors/light.yaml) | [sound_level_meter](https://github.com/stas-sl/esphome-sound-level-meter) |

### Sampling

A key part of the SAMBA configuration is the sampling routine. SAMBA is configured to constantly measure environmental parameters and then periodically summarise those measurements at a set interval before uploading them to the database.

Measurements are made at set frequencies

Some basic quality assurance is done on the measurements to ensure they are reliable.

Loop checks the template sensor

Data is sent to server.

The following table summarises the sampling process used in the default SAMBA configuration. Again, users are free to modify this but no support will be offered by the IEQ Lab.

| Table |  Here |
|: *** :|: *** :|
| value | value |

### Setup

Compiling the firmware is done by:

1.  Download esphome
2.  Run firmware file.
