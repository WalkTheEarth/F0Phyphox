#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "lib/phyphox/transport/Transport.h"

struct PhyphoxBleState;

/**
 * phyphox BLE transport for the Flipper Zero.
 *
 * Implements phyphox::Transport on top of a custom Flipper BLE profile
 * (services cddf0001 / cddf1001). The phyphox library core does the
 * experiment XML, transfer and packet handling.
 */
class FlipperTransport : public phyphox::Transport {
public:
    FlipperTransport();
    ~FlipperTransport() override;

    /** Restore the default Flipper BLE profile. Safe to call twice. */
    void stop();

    /** Drop the current connection so the phone reconnects and re-fetches the experiment. */
    void disconnect();

    bool is_advertising() const;
    bool has_error() const;

    // phyphox::Transport
    bool begin(const phyphox::GattLayout& layout, phyphox::TransportListener& listener) override;
    bool notify(phyphox::CharId id, const uint8_t* data, uint16_t len) override;
    void setValue(phyphox::CharId id, const uint8_t* data, uint16_t len) override;
    bool connected() const override;
    uint16_t mtuPayload() const override;
    void poll() override;
    void restartAdvertising() override;
    const char* name() const override {
        return "Flipper";
    }

private:
    PhyphoxBleState* state;
};
