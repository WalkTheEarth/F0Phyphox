# Phyphox Companion

Streams Flipper Zero GPIO sensors to the [phyphox](https://phyphox.org) phone
app over the Flipper's built-in Bluetooth LE. No WiFi devboard, no phone-side
plugins, no extra hardware beyond the sensor.

Supported sensors, chosen from the app's menu:

| Sensor | Values | phyphox channels |
| --- | --- | --- |
| DS18B20 1-Wire thermometer | temperature | 1 |
| DHT11 temperature/humidity | temperature, humidity | 2 |
| GPIO voltage (ADC) | voltage | 1 |

Up to four channels are streamed at once (one sensor of each type), matching
the phyphox BLE data packet.

## Wiring

### DS18B20

| DS18B20 | Flipper Zero GPIO |
| :-----: | :---------------: |
| VDD | 9 (3V3) |
| GND | 18 (or 8 / 11) |
| DQ | 17 (1-Wire) |

Pin 17 is the iButton / 1-Wire line and has a built-in pull-up, so no external
4.7 kΩ resistor is needed there. On any other GPIO pin, add a 4.7 kΩ pull-up
from DQ to 3V3. Do not use the 5 V pins (1 and 2) for power.

### DHT11

| DHT11 | Flipper Zero GPIO |
| :---: | :---------------: |
| VDD | 9 (3V3) |
| GND | 18 (or 8 / 11) |
| DATA | any GPIO (default suggestion: 2–7 or 15–17) |

Power the DHT11 from **3.3 V**, not 5 V: at 5 V its data line swings to 5 V.
Add a 4.7–10 kΩ pull-up from DATA to 3V3 (most DHT11 breakout modules already
include one). The Flipper's internal pull-up alone may work on short wires but
is weaker than the datasheet asks for.

### Voltage

Connect the signal to an ADC-capable pin and share GND:

| Header pin | Port | ADC channel |
| :---: | :---: | :---: |
| 2 | PA7 | 12 |
| 3 | PA6 | 11 |
| 4 | PA4 | 9 |
| 7 | PC3 | 4 |
| 15 | PC1 | 2 |
| 16 | PC0 | 1 |

The ADC is configured for a 0–2.048 V scale. Inputs above ~2.05 V read as
full scale (they do not damage the pin, but the value is wrong). To measure a
higher voltage, use a resistor divider and scale the reading on the phone:

```
Vin = Vpin * (R1 + R2) / R2
```

Do not exceed 3.3 V on any GPIO pin.

## Using it

1. Wire the sensor(s) and enable Bluetooth in Flipper Settings.
2. Open **Phyphox Companion** (Apps → Bluetooth). The Flipper advertises as
   `FLIP` as soon as the app opens, before streaming starts.
3. On the menu, press OK on a sensor row and choose its pin (or **Off**). The
   default configuration is a DS18B20 on pin 17.
4. In the phyphox app: **+ → Bluetooth device → FLIP**. The experiment can be
   downloaded now; it shows the current sensor selection.
5. Select **Start streaming** on the Flipper. Values begin arriving; the
   experiment has one graph and one value per configured channel.
6. Press Back on the Flipper to stop streaming and return to the menu. Press
   Back on the menu to exit and restore the normal Flipper BLE profile.

The sensor configuration lives in RAM and is reset when the app exits. Each
pin can belong to only one sensor, so pins already assigned to another sensor
are hidden in the picker. Changing a sensor assignment while a phone is
connected disconnects it so phyphox re-downloads the updated experiment;
reconnect with **Try again** or by selecting **FLIP** again.

## Build and install

The app is built with [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt)
against the official **dev** SDK (the one in `~/.ufbt/current`, API 88.3,
commit `ad3d55ba`). It was tested on a Flipper running **Unleashed**
`unlshd-094e` (API 88.19); the `.fap` loads and runs there as well. Your
Flipper needs firmware with API 88.3 or newer.

```sh
cd F0Phyphox
ufbt                 # builds dist/f0phyphox.fap
ufbt launch          # optional: Flipper connected over USB
```

Or copy `dist/f0phyphox.fap` to `SD:/apps/Bluetooth/` and start
**Phyphox Companion** from Apps → Bluetooth. The app id is still
`f0phyphox`, so the file name and install folder are unchanged.

Host self-test for the sensor conversions (any C compiler):

```sh
cc -I. tests/sensor_math_test.c -o /tmp/sensor_math_test -lm && /tmp/sensor_math_test
```

## Repository layout

| Path | Contents |
| --- | --- |
| `lib/phyphox/` | Vendored phyphox BLE 2.0 library: experiment builder, serializer, transfer/session core, transport interface. |
| `phyphox_ble.cpp` | Flipper Zero transport and custom BLE profile: experiment service `cddf0001`, data service `cddf1001`, control/data notifications. |
| `phyphox_app.cpp` | Sensor drivers, sampling scheduler, menu/status UI, and the phyphox experiment builder. |
| `sensor_math.h` | Pure decoding helpers shared with the host test. |
| `tests/sensor_math_test.c` | Assert-based host test for DS18B20/DHT11 decoding. |
| `application.fam` | Flipper app manifest. |
| `CLAUDE.md` | Architecture, pin map, and maintenance notes for agents/developers. |

## Notes and limits

- The advertised name is `FLIP`. The ST stack adds flags and TX-power AD
  elements itself, and the 128-bit service UUID is 18 bytes, leaving only
  five characters for the local name in the 31-byte legacy advertising
  packet. A longer name makes `aci_gap_set_discoverable` reject the packet and
  the Flipper never shows up in a scanner.
- The phyphox BLE data characteristic carries five `float32` values per
  notification. Unused channels are sent as zero. The app builds experiments
  with at most four channels (DS18B20 + DHT11 temperature + DHT11 humidity +
  voltage).
- One sensor of each type can be configured. Two DS18B20s on the same 1-Wire
  bus are not supported yet.
- While the app is open, it replaces the Flipper's default BLE serial profile,
  so the Flipper mobile app disconnects. The default profile is restored when
  the app exits. **Start streaming** only controls sensor sampling; the
  phyphox service stays up in the menu so a previously saved experiment can
  reconnect at any time.
- BLE must be enabled in Settings; otherwise the streaming screen shows
  `BLE: error` and the phone cannot connect.
- Streaming enables the 5 V OTG rail (`power_enable_otg`), as the original
  DS18B20 app did. The 3V3 pin does not need it, but it keeps existing 5 V
  sensor setups working.
- DS18B20: the power-on scratchpad reads exactly 85.0 °C and has a valid CRC;
  the driver rejects that exact scratchpad so a reset does not produce a
  false 85 °C spike. Negative temperatures are decoded from the raw 16-bit
  two's-complement register.
- DHT11: sampled every 2 s, bit-banged with interrupts disabled for the ~4 ms
  response. It needs a pull-up (see wiring). The DHT11 driver has been
  compiled and unit-tested but not yet verified on real hardware in this
  repository; if it fails, check the pull-up and the pulse-count threshold.
- If phyphox reports **"no phyphox service"** (notification for
  `cddf0002` could not be enabled), the phone connected while the Flipper was
  not running this app's BLE profile, or Android cached the old GATT table.
  Keep **Phyphox Companion** open (the menu is enough), then forget/re-add
  `FLIP` in the phone's Bluetooth settings and reconnect. Toggling phone
  Bluetooth off/on also clears a stale GATT cache.
- If BLE stops appearing, run the Flipper CLI `log` command while the app
  starts and look for `GapDriver: set_discoverable failed` or
  `GattChar: Failed to add`.
- The vendored library has one local portability change: two `0.5` constants
  in `lib/phyphox/core/Serializer.cpp` are cast to `double` because the
  Flipper build uses `-Wdouble-promotion` with `-fsingle-precision-constant`.

## Credits

- [phyphox-arduino](https://github.com/phyphox/phyphox-arduino) — phyphox BLE
  library, LGPL-3.0-or-later. Vendored at upstream commit `a08a3fb` (2.0.0).
  See `lib/phyphox/COPYING.LESSER`.
- [Flipper `example_thermo`](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/examples/example_thermo)
  — DS18B20 scratchpad/CRC/temperature code and the wiring above.
- [phyphox](https://phyphox.org) — the phone app and BLE protocol.
