# F0 Phyphox roadmap

## Current state

v1.0 is a single-sensor app: a DS18B20 on GPIO 17, sampled at about 1 Hz,
streamed to the phyphox app over the Flipper's built-in BLE. The phyphox BLE
2.0 library core is vendored; the Flipper transport and custom BLE profile
live in `phyphox_ble.cpp`.

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
  and pin conflicts matter.
- **The custom BLE profile/GATT APIs must exist on the target firmware.**
  Test both official and Unleashed builds.

## Architecture for more sensors

1. **Sensor interface** — `init`, `read`, `describe`: name, units, channel
   count, sample period, optional asynchronous conversion.
2. **Registry/probe** — 1-Wire ROM search, I2C scan, configured pin/bus
   profiles.
3. **Sensor manager** — schedules conversions and reads; owns the buses; no
   UI or BLE code.
4. **Experiment factory** — maps sensor values to a phyphox experiment:
   containers, graphs, values, units, views. Enforce the five-value budget.
5. **Config** — SD file with selected sensor(s), bus/pins, sample rate, and
   channel map; sensible built-in defaults.
6. **UI** — scan/select sensor, show live values, start/stop; keep the
   current single-sensor screen as the fallback.
7. **Transport** — the existing `FlipperTransport` stays generic; the
   library port is reusable.

## Phases

### v1.1 — Sensor abstraction + multiple DS18B20
- Add the `Sensor` interface and move the DS18B20 driver behind it.
- Use 1-Wire ROM search + Match ROM; support 1–5 DS18B20s on the same bus.
- One experiment with up to five graphs/values; show each sensor's ROM short ID.
- Handle CRC/presence, 750 ms conversions, and parasitic-power caveats.

### v1.2 — I2C framework + first drivers
- I2C on the external GPIO header; bus scan; pull-up and power notes.
- Drivers: BME280/BMP280 (temperature/pressure/humidity) and SHT3x/SHT4x
  (temperature/humidity).
- Prebuilt experiment templates per sensor; units and graph labels.
- Add SCD4x (CO2) and VEML7700/TSL2561 (light) once the first two are stable.

### v1.3 — Selection UI + SD config
- Sensor scan/select screen; sample-rate and channel mapping.
- Save/load config on SD; reset to defaults.
- Rebuild the experiment from the config and restart the BLE profile after a
  change.

### v1.4 — More buses
- SPI: MAX6675/MAX31855 thermocouple, MCP3008/ADS1x15 ADC.
- UART: PMS5003 particulate, MH-Z19 CO2, NMEA GPS (read-only).
- ADC/GPIO: analog sensors, pulse/frequency (anemometer, rain gauge),
  switches.
- Voltage/level-shift and power guidance per sensor.

### v2.0 — Multi-sensor and the five-channel limit
- For more than five values, first offer **channel selection**: pick which
  five values go to phyphox.
- Multiple experiment profiles (temperature, environment, air quality, …);
  switching restarts the BLE profile.
- Investigate a time-multiplexed custom experiment only if phyphox's
  analysis/config can demultiplex it; do not promise it yet.
- Phone inputs: sliders/buttons for sample rate, tare/zero, start/stop;
  phone sensors as inputs.

### v2.x — Tooling and polish
- CI: build with ufbt for the official dev and Unleashed API levels; host
  tests for the serializer core; release workflow.
- SD CSV logging for standalone runs with timestamps and export.
- MTU/throughput and connection-parameter tuning; reconnect behavior.
- Documentation: wiring per sensor, supported firmware, troubleshooting
  (`set_discoverable`, GATT errors, the FLIP name).
- Firmware compatibility matrix and release notes.

## Candidate sensors

| Bus | Candidates |
| --- | --- |
| 1-Wire | DS18B20, DS18S20, DS1822, MAX31820, DS2438; multiple DS18B20s |
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
