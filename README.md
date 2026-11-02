# F0 Phyphox

Streams a DS18B20 1-Wire thermometer to the **phyphox** app over the
Flipper Zero's built-in Bluetooth LE. No WiFi devboard, no phone-side
plugins, no extra hardware.

## Wiring

| DS18B20 | Flipper Zero GPIO |
| :-----: | :---------------: |
| VDD     | 9 (3V3)           |
| GND     | 18 (or 8 / 11)    |
| DQ      | 17 (1-Wire)       |

Pin 17 is the iButton / 1-Wire line and has a built-in pull-up, so no
external 4.7 kΩ resistor is needed. Do not use the 5 V pins (1 and 2).

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
**Phyphox DS18B20** from Apps → Bluetooth.

## Using it

1. Wire the sensor and enable Bluetooth in Flipper Settings.
2. Open **Phyphox DS18B20**. The screen shows `BLE: advertising`.
3. In the phyphox app: **+ → Bluetooth device → FLIP**.
4. The `DS18B20 Temperature` experiment opens with a live graph and value.

Press Back on the Flipper to exit. The app restores the normal Flipper
BLE profile on the way out (the Flipper mobile app reconnects afterwards).

## What is in here

| Path | Contents |
| --- | --- |
| `lib/phyphox/` | Vendored phyphox BLE 2.0 library: experiment builder, serializer, transfer/session core, transport interface. |
| `phyphox_ble.cpp` | Flipper Zero transport and custom BLE profile: experiment service `cddf0001`, data service `cddf1001`, control/data notifications. |
| `phyphox_app.cpp` | DS18B20 driver (based on Flipper's official `example_thermo`), phyphox server polling, and the screen. |
| `application.fam` | Flipper app manifest. |

The app is a straight port of the phyphox BLE protocol, not the WiFi/HTTP
protocol. The Flipper has no WiFi, and phyphox's Arduino library is the BLE
one; the Flipper's STM32WB radio provides the peripheral side.

## Notes / limits

- The advertised name is `FLIP`. The ST stack adds flags and TX-power AD
  elements itself, and the 128-bit service UUID is 18 bytes, leaving only
  five characters for the local name in the 31-byte legacy advertising
  packet. Four are used for a one-byte margin. A longer name makes
  `aci_gap_set_discoverable` reject the packet and the Flipper never shows up
  in a scanner (the Flipper GAP code still reports "advertising").
- While running, the app replaces the Flipper's default BLE serial profile,
  so the Flipper mobile app disconnects. It is restored on exit.
- BLE must be enabled in Settings; otherwise the app shows `BLE: starting`
  and never advertises.
- Confirmed on 2026-11-02 with a phone: FLIP advertises with service
  `cddf0001-30f7-4671-8b43-5e40ba53514a`, the experiment downloads, and live
  temperature arrives. The first attempt failed because a longer name
  overflowed the 31-byte ad packet; the name is now `FLIP`.
- If it ever stops appearing, run the Flipper CLI `log` command while the
  app starts and look for `GapDriver: set_discoverable failed` or
  `GattChar: Failed to add`.
- The DS18B20 powers up with 85.0 °C in its scratchpad. The app rejects the
  exact power-on scratchpad (factory TH/TL/config), so a sensor reset shows
  `measuring...` instead of a false 85 °C spike. Negative temperatures are
  decoded from the raw 16-bit two's-complement register.
- The vendored library has one local portability change: two `0.5` constants
  in `lib/phyphox/core/Serializer.cpp` are cast to `double` because the
  Flipper build uses `-Wdouble-promotion` with `-fsingle-precision-constant`.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for multi-sensor plans: multiple DS18B20s,
I2C/SPI/UART sensors, the selection/config UI, and the five-channel phyphox
BLE limit.

## Credits

- [phyphox-arduino](https://github.com/phyphox/phyphox-arduino) — phyphox BLE
  library, LGPL-3.0-or-later. Vendored at upstream commit `a08a3fb` (2.0.0).
  See `lib/phyphox/COPYING.LESSER`.
- [Flipper `example_thermo`](https://github.com/flipperdevices/flipperzero-firmware/tree/dev/applications/examples/example_thermo)
  — DS18B20 scratchpad/CRC/temperature code and the wiring above.
- [phyphox](https://phyphox.org) — the phone app and BLE protocol.
