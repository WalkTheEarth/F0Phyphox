# CLAUDE.md — Phyphox Companion

Agent/developer notes for this repository. Read this before changing the app.
The user-facing description and wiring live in `README.md`.

## What this is

A Flipper Zero FAP (`.fap`, appid `f0phyphox`, display name
**Phyphox Companion**) that streams GPIO sensor values to the phyphox phone
app over the Flipper's BLE radio. It is a BLE peripheral, not a WiFi/HTTP
device. The phyphox BLE 2.0 library is vendored in `lib/phyphox/`.

Supported today (configured from the app menu, one sensor of each type):

| Type | Pins | Values | Source |
| --- | --- | --- | --- |
| DS18B20 | any external GPIO, default 17 | temperature | `ds18b20_*` in `phyphox_app.cpp` |
| DHT11 | any external GPIO | temperature + humidity | `dht11_*` in `phyphox_app.cpp` |
| Voltage | ADC-capable pins 2, 3, 4, 7, 15, 16 | pin voltage | `voltage_*` in `phyphox_app.cpp` |

The DS18B20 path has been confirmed on hardware. The menu, DHT11 driver and
ADC voltage path are implemented and compile, but were not verified on a
physical Flipper in this repository; treat them as first-test candidates.

## Build / test

```sh
ufbt                 # build dist/f0phyphox.fap (host toolchain in ~/.ufbt/current)
ufbt launch          # optional, Flipper over USB

cc -I. tests/sensor_math_test.c -o /tmp/sensor_math_test -lm && /tmp/sensor_math_test
```

- SDK: official **dev** API 88.3 (`~/.ufbt/current`, commit `ad3d55ba`).
  Also runs on Unleashed API 88.19 (`unlshd-094e`).
- Target: Flipper Zero (f7). No host build of the app itself; the only host
  test is `sensor_math.h` + `tests/sensor_math_test.c`.
- Compile flags worth knowing: `-Wdouble-promotion` and
  `-fsingle-precision-constant`. Use `0.5f`, `0.001f`, etc. Never use `%f`
  with `snprintf` (newlib float printf is not available); format integers and
  scale manually.
- Stack: `application.fam` sets `stack_size=6*1024` because
  `server_start()`/`server_update_experiment()` build the experiment on the
  app main thread. Do not reduce it without checking the builder object sizes.

## Architecture

```
phyphox_app_main (app main thread, 6 KB stack)
  ├─ view_port (GUI thread) ── phyphox_draw()  menu / pin picker / live values
  │                             phyphox_input() pushes InputEvent into queue
  ├─ server_start() at app launch
  │    builds the experiment for the current config, starts the BLE profile
  │    and keeps it advertising while the menu is shown
  ├─ app loop (always 10 ms tick)
  │    ├─ Server::poll()                 BLE transfer/session servicing
  │    └─ if streaming: stream_tick()
  └─ stream_tick()
       ├─ ds18b20_request/read            non-blocking: request, read 800 ms later
       ├─ dht11_read                      blocking ~24 ms every 2 s
       ├─ furi_hal_adc_read               every 1 s
       └─ Server::writeFloats(5 floats)   one notification with all channels
```

BLE callbacks run in the BLE event thread (see `phyphox_ble.cpp`); the app
does not take locks, same as the original single-sensor design. `Server` and
`FlipperTransport` are not thread-safe by themselves; keep all `poll()` /
`writeFloats()` calls on the app main thread. The profile runs for the whole
app lifetime: phyphox reconnects to the last device and expects the service
even while the Flipper is still on the menu (this was the cause of the
"no phyphox service" error when the profile was only started at Start).

### App state

`AppContext` in `phyphox_app.cpp` holds everything:

- `config_pin[SensorTypeCount]`: selected Flipper header pin per sensor type,
  `0` = off. In RAM only; defaults to DS18B20 on pin 17.
- `ui_mode`: `UiMain` or `UiPinPick`.
- `streaming`: `false` → menu path, `true` → `stream_tick()` path. It only
  controls sampling and the screen; the BLE profile stays up either way.
- `ch_*`: phyphox data channel (1-based), computed by `compute_channels()`:
  DS18B20=1, DHT11 temperature=2, DHT11 humidity=3, voltage=4 when all are
  enabled; disabled sensors are skipped, so channels are compacted.
- Per-sensor runtime state: `ds_converting/attempted/valid/started`,
  `dht_next/attempted/valid`, `voltage_next/attempted`.

`server_start()` (called once from `app_alloc()`):

1. Placement-new a `phyphox::Server` into the preallocated `server_memory`,
   set the clock and `FLIP` name.
2. `compute_channels()` for the current config.
3. `server_update_experiment()`: build a `PhyphoxBleExperiment` with one graph
   + one value per channel. With no sensors selected it adds a single
   `InfoField` ("Select a sensor…"), so the served document stays valid.
4. `start()` — the transport comes up and advertising begins.

`server_update_experiment()` is also called when a pin is assigned in the
picker. The picker disconnects the phone first (`FlipperTransport::disconnect`)
so it reconnects and re-downloads the changed XML. `addExperiment()` after
`start()` is safe here because the experiment only uses output/data channels;
the GATT layout never changes. Verified on hardware: the new XML is served.

`stream_start()` only initializes the sensors:

1. Resolve pins via `pin_record()` (table `gpio_pins[]` from the SDK).
2. Init hardware:
   - DS18B20: `onewire_host_alloc(record->pin)` + `onewire_host_start`
   - DHT11: GPIO input with pull-up
   - Voltage: GPIO analog + `furi_hal_adc_acquire` + `furi_hal_adc_configure`
3. `power_enable_otg(true)`, reset sample timers, set `streaming = true`.

`stream_stop()` only tears down sampling: OneWire free, GPIO restore, ADC
release, OTG off, runtime flags reset. It leaves the server and BLE profile
running so the phone can stay connected or reconnect. `app_free()` calls
`server_stop()`, which stops the transport (restores the default Flipper BLE
profile) and destroys the server.

Data is only notified after the phone has subscribed
(`Server::subscribed()`), and only after every enabled sensor has completed
its first attempt (`*_attempted`), so a working DS18B20 does not send a 0 °C
spike while its first 800 ms conversion runs. A failed sensor does not block
the others.

## Hardware / pin map

`GpioPinRecord` has `pin`, `name`, `channel`, `number` (header pin number,
0 = not on the external header). Iterate `gpio_pins[]` / `gpio_pins_count`
instead of hardcoding pointers. From firmware `furi_hal_resources.c`:

| Header pin | Port | ADC channel | Notes |
| :---: | :---: | :---: | --- |
| 2 | PA7 | 12 | |
| 3 | PA6 | 11 | |
| 4 | PA4 | 9 | |
| 5 | PB3 | none | |
| 6 | PB2 | none | |
| 7 | PC3 | 4 | |
| 10 | PA14 | none | SWCLK |
| 12 | PA13 | none | SWDIO |
| 13 | PB6 | none | USART TX |
| 14 | PB7 | none | USART RX |
| 15 | PC1 | 2 | |
| 16 | PC0 | 1 | |
| 17 | PB14 | none | iButton/1-Wire, built-in pull-up |

Power: header 9 = 3V3, 1/2 = 5 V (OTG-controlled), 8/11/18 = GND.

Sensor notes:

- **DS18B20**: 12-bit conversion takes 750 ms; code waits 800 ms. Uses
  `SKIP_ROM`, so exactly one DS18B20 per pin. Rejects the exact power-on
  scratchpad (85 °C, factory TH/TL/config) because its CRC is valid; negative
  values are decoded as `int16_t / 16.0f`. See `sensor_math.h`.
- **DHT11**: host pulls low 20 ms, releases, then reads 80 µs low + 80 µs high
  + 40 bits (50 µs low, then 26–28 µs high = 0 or 70 µs high = 1). The code
  counts loop iterations: `high > low` is the bit decision, so it does not
  depend on CPU clock. A stuck line is bounded by `DHT11_PULSE_LIMIT`. Timing
  runs inside `FURI_CRITICAL_ENTER`, ~4 ms per sample, once per 2 s. Needs an
  external 4.7–10 kΩ pull-up for reliable operation (breakout boards usually
  have one). Powered at 3.3 V to keep DATA at 3.3 V.
- **Voltage**: default ADC config is scale 2.048 V, 12-bit, 64× oversampling.
  `furi_hal_adc_convert_to_voltage()` returns mV; the app stores volts. Above
  ~2.05 V the reading saturates. A resistor divider is required for higher
  voltages; the app reports the pin voltage, not the divider's input.

## phyphox BLE protocol constraints

- Data characteristic carries **five float32** per notification
  (`PHYPHOX_BLE_DATA_CHANNELS`). Unused values are zero. This is the hard
  channel budget; there are currently four channels (DS18B20 + DHT11 ×2 +
  voltage).
- Advertised name must stay **5 characters or fewer**. The 31-byte legacy ad
  packet minus the 128-bit service UUID leaves 5 characters. The app uses
  `FLIP`; `phyphox_ble.cpp` logs "Name too long..." and the stack rejects the
  packet silently if this is exceeded. Do not "improve" the name.
- GATT layout: experiment service `cddf0001-…`, data service `cddf1001-…`.
  UUIDs are stored Little Endian in `phyphox_ble.cpp`.
- The custom profile replaces the Flipper serial profile for the whole app
  lifetime; `FlipperTransport::stop()` (called from `server_stop()` on exit)
  restores it. BLE must be enabled in Settings. `FLIP` advertises while the
  menu is shown as well as while streaming; **Start streaming** only starts
  sampling. Do not move the profile start back into `stream_start()` —
  phyphox reconnects to a saved device while the menu is up and reports
  "no phyphox service" if the profile is absent.
- Changing a sensor assignment while a phone is connected calls
  `FlipperTransport::disconnect()` and then `server_update_experiment()`, so
  the phone reconnects and re-downloads the changed XML.
- `writeFloats()` always sends 5 floats regardless of `count` (the library
  fills the rest with zero at the transport layer). Experiment elements must
  only reference channels that have graphs/values.

## Adding a sensor type

1. Add an enum value to `SensorType` and a name to `kSensorNames`.
2. Add any per-sensor runtime fields to `AppContext` and reset them in
   `stream_stop()`.
3. Add a driver near the other drivers. Keep blocking work bounded.
4. Init in `stream_start()` (pin lookup via `pin_record`, bus allocation).
5. Assign a channel in `compute_channels()` and add a graph/value in
   `server_update_experiment()`. Keep both, the `values[]` mapping in
   `stream_tick()` and `draw_stream()` in sync.
6. Sample from `stream_tick()` with a `next_*` timestamp; set `new_data` on
   success and `*_attempted` after the first attempt.
7. Add the pin filter in `open_pin_picker()` if the sensor needs special pins
   (the voltage type shows the ADC-channel filter as an example).
8. Update the `values[]` mapping and the live screen in `draw_stream()`.
9. Update `README.md`, this file, and `ROADMAP.md`.

There are four channel slots used; adding a fifth value means dropping one of
the existing ones or redesigning channel selection.

## UI behavior

- Main menu: three sensor rows (`name pin N` / `name off`) + **Start
  streaming**. OK on a row opens the pin picker; OK on Start begins streaming.
  Up/Down wrap. Back exits.
- Pin picker: `Off` + every external GPIO (ADC-capable only for Voltage),
  skipping pins assigned to another sensor type. Selecting a pin writes
  `config_pin[]`. Header shows `"<type> pin"`.
- Streaming screen: BLE status plus one line per enabled sensor
  (`measuring…`, `no sensor`, or the value). Back stops streaming and returns
  to the menu (it does not exit the app).
- Drawing uses one full-screen `ViewPort`; there is no Flipper `Menu`
  service. All coordinates are hand-placed for 128×64. Rebuild after UI edits.
- `view_port_update()` is called from the app main thread; the draw callback
  runs in the GUI thread. Display values are `volatile`.

## Pitfalls already hit (do not reintroduce)

- Float formatting: `%f` is unavailable. Use `format_tenths()` / integer
  millivolts.
- `-fsingle-precision-constant`: write `0.5f`, not `0.5`.
- DHT11 partial reads: only decode after all 40 bits were counted, otherwise
  an all-zero buffer passes the checksum (`0 == 0`) and reports 0 °C/0 %.
- DS18B20 power-on scratchpad: CRC is valid, so it must be rejected by bytes.
- The advertising name and the 31-byte packet limit.
- Label strings passed to `PhyphoxBleExperiment` must outlive the server
  (they are read when the phone downloads the experiment, not at
  `addExperiment`). Use string literals or `AppContext` buffers. Current code
  uses literals only.
- OneWire operations are wrapped in `FURI_CRITICAL_ENTER`; keep them short.
- `power_enable_otg()` needs `RECORD_POWER`; it is opened once in
  `app_alloc()` and closed in `app_free()`.
- The BLE server starts in `app_alloc()` and must keep running while the menu
  is shown. Starting it only in `stream_start()` caused phyphox to connect to
  the default serial profile and fail with "no phyphox service". The menu loop
  polls `Server::poll()` every 10 ms for the same reason.
- Starting a new stream calls `stream_stop()` first, which now only tears down
  sensors; it must not stop the transport or destroy the server.
- The old worker thread was removed; everything runs on the app main thread.
  Don't block longer than a few tens of ms outside the sample calls or BLE
  polling stalls.

## Debugging on hardware

- Flipper CLI `log` while launching shows the `PhyphoxBle` tag and BT status.
- `GapDriver: set_discoverable failed` → advertising packet overflow (name).
- `GattChar: Failed to add` → GATT profile problem.
- "no phyphox service" / notification for `cddf0002` could not be enabled →
  the phone connected while the custom profile was not running, or its GATT
  cache is stale. The profile now runs whenever the app is open; still,
  forgetting/re-adding `FLIP` on the phone or toggling phone Bluetooth clears
  an old cache. Verified from a Linux host: while the menu is shown, FLIP
  advertises and `cddf0001`/`0002`/`1002` are present; subscribing to
  `cddf0002` transfers the XML, and `cddf1002` delivers live floats.
- No experiment in phyphox → verify the phone sees `FLIP` and the experiment
  characteristic notifies; the phyphox app downloads the XML on connect.
- Values stay at zero → check the sensor line, pull-up and pin selection; the
  streaming screen is the primary diagnostic (`no sensor` / `measuring…`).
- Voltage reads 2.048 → input at/above full scale; use a divider.

## Vendored library

`lib/phyphox/` is upstream phyphox-arduino commit `a08a3fb` (2.0.0), LGPL.
Only known local change: `0.5` → `0.5f` casts in
`lib/phyphox/core/Serializer.cpp` for the Flipper warning flags. Prefer
changes in the app/transport layer; keep the library diff minimal.
