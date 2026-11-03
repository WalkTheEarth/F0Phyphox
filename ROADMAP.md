# Phyphox Companion roadmap

## Current state

v2.0 supports three sensor types selected from an on-device menu:

- DS18B20 on any GPIO (default pin 17), temperature.
- DHT11 on any GPIO, temperature and humidity.
- GPIO voltage on the ADC-capable pins 2, 3, 4, 7, 15, 16.

One sensor of each type can run at once, for four phyphox channels. The
selection lives in RAM. The phyphox BLE 2.0 library core is vendored; the
Flipper transport and custom BLE profile live in `phyphox_ble.cpp`.

## Hard constraints

- **The Flipper has no WiFi.** This project is BLE-only.
- **The phyphox BLE data characteristic carries five `float32` values per
  notification.** That is the channel budget for one experiment: CH1..CH5.
  More values need channel selection, multiple profiles, or a phyphox-side
  experiment.
- **The legacy advertising packet is 31 bytes.** With the 128-bit phyphox
  service UUID, the local name has about five characters (`FLIP`).
- **One BLE connection and one profile at a time.** Changing the experiment
  or GATT layout means restarting the BLE profile.
- **Flipper GPIO is 3.3 V.** 5 V sensors need level shifting; power budget
  and pin conflicts matter. The ADC scale is 2.048 V.
- **The custom BLE profile/GATT APIs must exist on the target firmware.**
  Test both official and Unleashed builds.

## Next

### Multiple DS18B20s
- 1-Wire ROM search + Match ROM; support 1–4 DS18B20s on one pin.
- One temperature channel per device, within the five-channel budget.
- Show each sensor's ROM short ID in the UI.
- Handle CRC/presence, 750 ms conversions, and parasitic-power caveats.

### More buses and drivers
- I2C on the external GPIO header: BME280/BMP280, SHT3x/SHT4x.
- Add SCD4x (CO2) and VEML7700/TSL2561 (light) once the first two are stable.
- SPI: MAX6675/MAX31855 thermocouple, MCP3008/ADS1x15 ADC.
- UART: PMS5003 particulate, MH-Z19 CO2, NMEA GPS (read-only).
- Pulse/frequency inputs (anemometer, rain gauge), switches.

### Configuration and data
- Save/load the sensor/pin selection on SD; reset to defaults.
- Optional SD CSV logging with timestamps.
- Channel selection when more than five values are configured.
- Multiple experiment profiles (temperature, environment, air quality, …).

## Polish

- CI: build with ufbt for the official dev and Unleashed API levels; host
  tests for `sensor_math.h` and the serializer core; release workflow.
- MTU/throughput and connection-parameter tuning; reconnect behavior.
- Documentation: wiring per sensor, supported firmware, troubleshooting
  (`set_discoverable`, GATT errors, the FLIP name).
- Firmware compatibility matrix and release notes.

## Candidate sensors

| Bus | Candidates |
| --- | --- |
| 1-Wire | DS18S20, DS1822, MAX31820, DS2438; multiple DS18B20s |
| I2C | BME280/BMP280, SHT3x/SHT4x, SCD4x, ENS160, CCS811, VEML7700, TSL2561, MLX90614, INA219, ADS1115, MPU6050, QMC5883 |
| SPI | MAX6675/MAX31855, MCP3008, ADS1256 |
| UART | PMS5003, MH-Z19, NMEA GPS |
| ADC/GPIO | analog temperature/light, pulse/frequency, switches |

Prioritize sensors that run at 3.3 V with a small driver. For 5 V sensors,
require level shifting and document the wiring.

## Non-goals

- WiFi/HTTP phyphox sensors; the Flipper has no WiFi.
- More than five simultaneous data channels without a phyphox-side experiment
  or profile switching.
- High-rate streaming (audio/IMU) over the Flipper BLE link.
- Arbitrary BLE central mode or acting as a phone-sensor bridge.

## Risks

- Custom BLE profile/GATT API availability and behavior differ across
  firmware forks and versions.
- Power, voltage, and pin conflicts; a wrong 5 V connection can damage the
  Flipper or the sensor.
- App size/heap if many drivers are linked; keep drivers small and probe at
  runtime.
- Phone-side variation (Android/iOS) in BLE discovery, MTU, and notifications.
- The 31-byte advertising limit; keep the local name short.
- The DHT11 bit-banged driver is timing-sensitive; an external pull-up and
  short wires are required for reliable reads.
