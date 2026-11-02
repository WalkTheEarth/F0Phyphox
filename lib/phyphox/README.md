Vendored from https://github.com/phyphox/phyphox-arduino
Upstream commit: a08a3fb76f9d05e421a9c456a681e6cac9b640e9 (library version 2.0.0)
License: GNU Lesser General Public License v3.0 or later — see COPYING.LESSER
and COPYING.

Included: the Arduino-independent core (Config, Crc32, ChannelStore,
ExperimentStore, Errors, Serializer, Server, Sink, Transfer), the experiment
builder (phyphoxBleExperiment), and the transport interface
(transport/Transport.h).

Not included: phyphoxBle.cpp (the Arduino façade) and the ArduinoBLE / ESP32 /
NINA-B31 transports. The Flipper transport lives in ../../phyphox_ble.cpp.

Local changes:
- core/Serializer.cpp: two `0.5` constants are cast to `(double)` to avoid
  -Wdouble-promotion under the Flipper build's -fsingle-precision-constant.
