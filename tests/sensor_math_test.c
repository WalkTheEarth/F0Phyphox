#include <assert.h>
#include <math.h>
#include <stdint.h>

#include "sensor_math.h"

int main(void) {
    /* DS18B20: 0x0191 = 25.0625 C, 0xFFF8 = -0.5 C, 0x0550 = power-on 85 C. */
    assert(fabsf(ds18b20_raw_to_celsius(0x0191) - 25.0625f) < 0.0001f);
    assert(fabsf(ds18b20_raw_to_celsius((int16_t)0xFFF8) + 0.5f) < 0.0001f);

    const uint8_t power_on[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    assert(ds18b20_is_power_on(power_on));
    uint8_t real_reading[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x00};
    real_reading[0] = 0x91;
    real_reading[1] = 0x01;
    assert(!ds18b20_is_power_on(real_reading));

    /* DHT11: 42 % RH, 23.0 C, checksum 65. */
    const uint8_t dht_ok[5] = {42, 0, 23, 0, 65};
    float humidity = 0, temperature = 0;
    assert(dht11_decode(dht_ok, &humidity, &temperature));
    assert(fabsf(humidity - 42.0f) < 0.0001f);
    assert(fabsf(temperature - 23.0f) < 0.0001f);

    const uint8_t dht_bad[5] = {42, 0, 23, 0, 64};
    assert(!dht11_decode(dht_bad, &humidity, &temperature));

    /* DHT22-style negative temperature: -1.5 C. */
    const uint8_t dht_negative[5] = {50, 0, 1, 0x80 | 5, 184};
    assert(dht11_decode(dht_negative, &humidity, &temperature));
    assert(fabsf(temperature + 1.5f) < 0.0001f);

    return 0;
}
