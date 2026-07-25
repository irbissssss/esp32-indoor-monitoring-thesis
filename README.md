# ESP32 Indoor Temperature and Humidity Monitoring System

Bachelor's thesis project for Metropolia University of Applied Sciences.

## Thesis

**Finnish title**

ESP32-pohjaisen sisäympäristön lämpötila- ja kosteusmittausjärjestelmän prototyypin kehittäminen

**English title**

Development of an ESP32-Based Prototype for Indoor Temperature and Humidity Monitoring

**Author**

Artem Leontev

## Project description

This project implements a local indoor environmental monitoring system based on ESP32 microcontrollers.

The prototype consists of:

- one ESP32-S3 main hub
- two ESP32-C3 wireless sensor nodes
- two SHT31 temperature and humidity sensors
- DS3231 real-time clock
- OLED display
- local web dashboard
- CSV data logging

The sensor nodes periodically measure temperature and humidity and transmit the readings to the main hub using ESP-NOW. The main hub timestamps the measurements, stores them locally, and provides a browser-based dashboard over a local Wi-Fi network.

No cloud services are required.

## Repository structure

```text
main-hub/
    NordicHomeMainHub.ino

sensor-node-kitchen/
    NordicHomeNode1.ino

sensor-node-bedroom/
    NordicHomeNode2.ino

docs/

images/

test-data/
```

## Hardware

- ESP32-S3 DevKit
- 2 × ESP32-C3 SuperMini
- 2 × SHT31 sensors
- DS3231 RTC
- OLED display

## System architecture

![System architecture](images/ESP32%20Sensorisolmu%20Data%20Flow-2026-07-24-160329.png)

## Software

- Arduino IDE
- ESP32 Arduino Core
- ESP-NOW
- LittleFS
- HTML / CSS / JavaScript
- Git & GitHub

## Current status

Completed:

- Main hub firmware
- Kitchen sensor node firmware
- Bedroom sensor node firmware
- ESP-NOW communication
- Local dashboard
- CSV logging

Planned:

- 24-hour system stability test
- Test data upload
- Hardware photographs
- Dashboard screenshots

## License

See the LICENSE file.