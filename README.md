# SAMBA IEQ Monitoring System

This repository contains all the firmware files for SAMBA v2. The SAMBA system was developed by the [IEQ Lab](https://www.sydney.edu.au/architecture/our-research/research-labs-and-facilities/indoor-environmental-quality-lab.html) at The University of Sydney. The latest version runs on [ESPHome](https://esphome.io), which is an actively-maintained open source system designed to control microcontrollers for home automation tasks.

### Structure

ESPHome devices are configured using [YAML](https://yaml.org). YAML is commonly used for configuration files as it is human-readable. Settings are expressed as key-value pairs. There are many [YAML guides](https://www.cloudbees.com/blog/yaml-tutorial-everything-you-need-get-started) that you can follow if you are unfamiliar.

The repository is structured to allow easy modification of the configuration files using YAML. ESPHome expects config files in the `esphome` directory and additional components in the `esphome/components` subdirectory. The repository is structured accordingly.

```         
├── esphome
|   ├── base
|   ├── components
|   ├── measure
|   ├── peripherals
|   ├── sensors
|   └── ...
```

The directory structure is based on subdirectories that group individual config files based on their type. For example, the configuration files for all sensor components are in the `sensors` subdirectory. `base` has config files for the board, wifi, over-the-air updates, and home assistant. `periphals` includes the real-time clock, analog to digital converter, and LEDs. And `measure` includes the script that handles the measurement loop and the config file for the InfluxDB upload.

### Sensors

The following sensors are installed on SAMBA and configured using .yaml files in the `sensors` subdirectory. All of the sensors are natively supported by ESPHome through what are known as .components'.

|     Parameter     |                              Sensor                              |                                         Config                                         |                        Component                         |
|:--------------------:|:---------------:|:---------------:|:---------------:|
|  Air Temperature  | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [thermal.yaml](https://github.com/IEQLab/samba/blob/b07876be9d153c4315995ed3d519412e2f8a302a/esphome/sensors/thermal.yaml#L9-L23) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |
| Relative Humidity | [Sensirion SHT40](https://sensirion.com/products/catalog/SHT40/) | [thermal.yaml](https://github.com/IEQLab/samba/blob/b07876be9d153c4315995ed3d519412e2f8a302a/esphome/sensors/thermal.yaml#L25-L36) | [sht4x](https://esphome.io/components/sensor/sht4x.html) |

### Sampling

A key part of the SAMBA configuration is the sampling routine.

### Setup

Compiling the firmware is done by:

1.  Download esphome
2.  Run firmware file.
