#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pure conversions shared by the firmware drivers and tests/sensor_math_test.c.
 * Keep this header free of Flipper includes so it can be compiled on a host. */

/* The DS18B20 temperature register is 16-bit two's complement with 0.0625 C
 * per LSB at 12-bit resolution. Cast the raw value to int16_t first: the
 * integer/decimal split in Flipper's example_thermo stores the integer part
 * in uint8_t and turns -1.0 C into -255.0 C. */
static inline float ds18b20_raw_to_celsius(int16_t raw) {
    return raw / 16.0f;
}

/* A conversion that never ran leaves the power-on scratchpad: 85.0 C and
 * factory TH/TL/config bytes. Its CRC is valid, so reject the exact bytes. */
static inline bool ds18b20_is_power_on(const uint8_t scratchpad[9]) {
    return scratchpad[0] == 0x50 && scratchpad[1] == 0x05 &&
           scratchpad[2] == 0x4B && scratchpad[3] == 0x46 &&
           scratchpad[4] == 0x7F && scratchpad[5] == 0xFF &&
           scratchpad[6] == 0x0C && scratchpad[7] == 0x10;
}

/* DHT11: humidity integer, humidity decimal, temperature integer,
 * temperature decimal (bit 7 is the sign on DHT22), checksum. */
static inline bool dht11_decode(
    const uint8_t data[5],
    float* humidity,
    float* temperature) {
    if((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) return false;
    *humidity = (float)data[0] + (float)data[1] * 0.1f;
    const float temp = (float)data[2] + (float)(data[3] & 0x7F) * 0.1f;
    *temperature = (data[3] & 0x80) ? -temp : temp;
    return true;
}
