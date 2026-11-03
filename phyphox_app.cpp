#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <power/power_service/power.h>
#include <one_wire/maxim_crc.h>
#include <one_wire/one_wire_host.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phyphox_ble.h"
#include "sensor_math.h"
#include "lib/phyphox/core/Server.h"
#include "lib/phyphox/phyphoxBleExperiment.h"

#define UI_TICK_MS             10
#define DS18B20_CONVERSION_MS  800U
#define DHT11_PERIOD_MS        2000U
#define VOLTAGE_PERIOD_MS      1000U
#define MAX_PIN_OPTIONS        20
#define PHYPHOX_VALUES         5

#define DS18B20_CMD_SKIP_ROM        0xccU
#define DS18B20_CMD_CONVERT         0x44U
#define DS18B20_CMD_READ_SCRATCHPAD 0xbeU

/* A pulse is at most ~80 us. The limit is only a stuck-line guard. */
#define DHT11_PULSE_LIMIT 8000U

typedef union {
    struct {
        uint8_t temp_lsb;
        uint8_t temp_msb;
        uint8_t user_alarm_high;
        uint8_t user_alarm_low;
        uint8_t config;
        uint8_t reserved[3];
        uint8_t crc;
    } fields;
    uint8_t bytes[9];
} DS18B20Scratchpad;

typedef enum {
    SensorDS18B20,
    SensorDHT11,
    SensorVoltage,
    SensorTypeCount,
} SensorType;

static const char* const kSensorNames[SensorTypeCount] = {"DS18B20", "DHT11", "Voltage"};

typedef enum {
    UiMain,
    UiPinPick,
} UiMode;

typedef struct {
    Gui* gui;
    ViewPort* viewport;
    FuriMessageQueue* queue;
    FlipperTransport* transport;
    void* server_memory;
    phyphox::Server* server;
    Power* power;

    uint8_t config_pin[SensorTypeCount]; // Flipper header pin number, 0 = off
    uint8_t selected;
    UiMode ui_mode;
    SensorType pin_type;
    uint8_t pin_count;
    uint8_t pin_selected;
    uint8_t pin_options[MAX_PIN_OPTIONS];
    char message[24];

    bool streaming;
    bool notifying;
    bool otg_enabled;

    OneWireHost* onewire;
    bool ds_converting;
    bool ds_attempted;
    uint32_t ds_started;
    volatile float ds_temp;
    volatile bool ds_valid;

    uint32_t dht_next;
    bool dht_attempted;
    volatile float dht_temp;
    volatile float dht_humidity;
    volatile bool dht_valid;

    FuriHalAdcHandle* adc;
    FuriHalAdcChannel voltage_channel;
    uint32_t voltage_next;
    bool voltage_attempted;
    volatile float voltage;

    uint8_t ch_ds;
    uint8_t ch_dht_temp;
    uint8_t ch_dht_humidity;
    uint8_t ch_voltage;
} AppContext;

static uint32_t phyphox_millis(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency());
}

static const GpioPinRecord* pin_record(uint8_t number) {
    if(number == 0) return NULL; // 0 means "off", not an internal pin
    for(size_t i = 0; i < gpio_pins_count; i++) {
        if(gpio_pins[i].number == number) return &gpio_pins[i];
    }
    return NULL;
}

/* ---------------------------------------------------------------- DS18B20 */

static void ds18b20_request(OneWireHost* onewire) {
    FURI_CRITICAL_ENTER();
    if(onewire_host_reset(onewire)) {
        onewire_host_write(onewire, DS18B20_CMD_SKIP_ROM);
        onewire_host_write(onewire, DS18B20_CMD_CONVERT);
    }
    FURI_CRITICAL_EXIT();
}

static bool ds18b20_read(OneWireHost* onewire, float* temperature) {
    bool success = false;

    FURI_CRITICAL_ENTER();
    do {
        DS18B20Scratchpad buf;
        size_t attempts_left = 10;

        do {
            if(!onewire_host_reset(onewire)) continue;
            onewire_host_write(onewire, DS18B20_CMD_SKIP_ROM);
            onewire_host_write(onewire, DS18B20_CMD_READ_SCRATCHPAD);
            onewire_host_read_bytes(onewire, buf.bytes, sizeof(buf.bytes));
            if(maxim_crc8(buf.bytes, sizeof(buf.bytes) - 1, MAXIM_CRC8_INIT) ==
               buf.fields.crc)
                break;
        } while(--attempts_left);

        if(attempts_left == 0) break;
        if(ds18b20_is_power_on(buf.bytes)) break;

        const int16_t raw =
            (int16_t)(((uint16_t)buf.fields.temp_msb << 8) | buf.fields.temp_lsb);
        *temperature = ds18b20_raw_to_celsius(raw);
        success = true;
    } while(false);
    FURI_CRITICAL_EXIT();

    return success;
}

/* ---------------------------------------------------------------- DHT11 */

/* The DHT11 is bit-banged: the host pulls the line low for >= 18 ms, then
 * releases it. The sensor answers with 80 us low, 80 us high, then 40 bits.
 * Every bit is 50 us low followed by 26-28 us high (0) or 70 us high (1).
 * Counting loop iterations in a critical section is self-calibrating: the
 * high pulse is compared with the 50 us low pulse that precedes it, so the
 * threshold does not depend on the CPU clock. */
static bool dht11_wait(const GpioPin* pin, bool level, uint32_t timeout_us) {
    while(furi_hal_gpio_read(pin) != level) {
        if(timeout_us == 0) return false;
        timeout_us--;
        furi_delay_us(1);
    }
    return true;
}

static bool dht11_count(const GpioPin* pin, bool level, uint32_t* count) {
    uint32_t n = 0;
    while(furi_hal_gpio_read(pin) == level) {
        if(++n > DHT11_PULSE_LIMIT) return false;
    }
    *count = n;
    return true;
}

static bool dht11_read(const GpioPin* pin, float* temperature, float* humidity) {
    uint8_t data[5] = {0};

    furi_hal_gpio_init(pin, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_write(pin, false);
    furi_delay_ms(20);
    furi_hal_gpio_write(pin, true);

    bool valid = false;
    FURI_CRITICAL_ENTER();
    furi_hal_gpio_init(pin, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);

    do {
        if(!dht11_wait(pin, false, 200)) break; // sensor pulls the line low
        if(!dht11_wait(pin, true, 200)) break;  // 80 us response low
        if(!dht11_wait(pin, false, 200)) break; // 80 us response high, first bit starts

        bool bits_ok = true;
        for(uint8_t i = 0; i < 40; i++) {
            uint32_t low, high;
            if(!dht11_count(pin, false, &low) || !dht11_count(pin, true, &high)) {
                bits_ok = false;
                break;
            }
            data[i >> 3] = (uint8_t)((data[i >> 3] << 1) | (high > low));
        }

        if(bits_ok) valid = dht11_decode(data, humidity, temperature);
    } while(false);

    FURI_CRITICAL_EXIT();
    furi_hal_gpio_init(pin, GpioModeInput, GpioPullUp, GpioSpeedLow);
    return valid;
}

/* ---------------------------------------------------------------- voltage */

static void voltage_start(AppContext* app, const GpioPinRecord* record) {
    app->voltage_channel = record->channel;
    furi_hal_gpio_init(record->pin, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    app->adc = furi_hal_adc_acquire();
    furi_hal_adc_configure(app->adc);
}

static float voltage_read(AppContext* app) {
    const uint16_t raw = furi_hal_adc_read(app->adc, app->voltage_channel);
    return furi_hal_adc_convert_to_voltage(app->adc, raw) * 0.001f;
}

/* ---------------------------------------------------------------- experiment */

static void compute_channels(AppContext* app) {
    uint8_t channel = 0;
    app->ch_ds = app->config_pin[SensorDS18B20] ? ++channel : 0;
    app->ch_dht_temp = app->config_pin[SensorDHT11] ? ++channel : 0;
    app->ch_dht_humidity = app->config_pin[SensorDHT11] ? ++channel : 0;
    app->ch_voltage = app->config_pin[SensorVoltage] ? ++channel : 0;
}

static void server_update_experiment(AppContext* app) {
    PhyphoxBleExperiment experiment(
        "Phyphox Companion",
        "Flipper Zero",
        "Live readings from sensors on the Flipper Zero GPIO.");

    PhyphoxBleExperiment::View view("Sensors");
    PhyphoxBleExperiment::Graph graphs[4];
    PhyphoxBleExperiment::Value values[4];
    uint8_t element = 0;

    auto add_channel = [&](uint8_t ch, const char* label, const char* unit, int precision) {
        graphs[element].setLabel(label);
        graphs[element].setChannel(0, ch);
        graphs[element].setLabelX("Time");
        graphs[element].setUnitX("s");
        graphs[element].setLabelY(label);
        graphs[element].setUnitY(unit);
        values[element].setLabel(label);
        values[element].setChannel(ch);
        values[element].setUnit(unit);
        values[element].setPrecision(precision);
        view.addElement(graphs[element]);
        view.addElement(values[element]);
        element++;
    };

    if(app->ch_ds) add_channel(app->ch_ds, "DS18B20 temperature", "C", 1);
    if(app->ch_dht_temp) add_channel(app->ch_dht_temp, "DHT11 temperature", "C", 1);
    if(app->ch_dht_humidity) add_channel(app->ch_dht_humidity, "DHT11 humidity", "%", 0);
    if(app->ch_voltage) add_channel(app->ch_voltage, "Voltage", "V", 3);

    if(element == 0) {
        PhyphoxBleExperiment::InfoField info("Select a sensor on the Flipper, then press Start.");
        view.addElement(info);
    }

    experiment.addView(view);
    app->server->addExperiment(experiment);
}

/* The phyphox profile runs for the whole app lifetime, menu included. phyphox
 * reconnects to the last device and needs the service to be present even when
 * streaming has not been started yet. */
static void server_start(AppContext* app) {
    app->server = new(app->server_memory) phyphox::Server(*app->transport);
    app->server->setClock(phyphox_millis);
    app->server->setDeviceName("FLIP");
    compute_channels(app);
    server_update_experiment(app);
    app->server->start();
}

static void server_stop(AppContext* app) {
    if(app->transport) app->transport->stop();
    if(app->server) {
        app->server->~Server();
        app->server = NULL;
    }
}

/* ---------------------------------------------------------------- streaming */

static void stream_stop(AppContext* app) {
    app->streaming = false;

    if(app->onewire) {
        onewire_host_stop(app->onewire);
        onewire_host_free(app->onewire);
        app->onewire = NULL;
    }

    const GpioPinRecord* dht = pin_record(app->config_pin[SensorDHT11]);
    if(dht) furi_hal_gpio_init_simple(dht->pin, GpioModeInput);

    const GpioPinRecord* voltage = pin_record(app->config_pin[SensorVoltage]);
    if(voltage) furi_hal_gpio_init_simple(voltage->pin, GpioModeInput);

    if(app->adc) {
        furi_hal_adc_release(app->adc);
        app->adc = NULL;
    }

    if(app->otg_enabled) {
        power_enable_otg(app->power, false);
        app->otg_enabled = false;
    }

    app->notifying = false;
    app->ds_converting = false;
    app->ds_attempted = false;
    app->ds_valid = false;
    app->ds_temp = 0;
    app->dht_attempted = false;
    app->dht_valid = false;
    app->dht_temp = 0;
    app->dht_humidity = 0;
    app->voltage_attempted = false;
    app->voltage = 0;
}

static void stream_tick(AppContext* app) {
    app->server->poll();

    const uint32_t now = phyphox_millis();
    bool new_data = false;

    if(app->ch_ds) {
        if(!app->ds_converting) {
            ds18b20_request(app->onewire);
            app->ds_converting = true;
            app->ds_started = now;
        } else if((uint32_t)(now - app->ds_started) >= DS18B20_CONVERSION_MS) {
            float temperature;
            const bool ok = ds18b20_read(app->onewire, &temperature);
            if(ok) {
                app->ds_temp = temperature;
                new_data = true;
            }
            app->ds_valid = ok;
            app->ds_attempted = true;
            app->ds_converting = false;
        }
    }

    if(app->ch_dht_temp) {
        const GpioPinRecord* dht = pin_record(app->config_pin[SensorDHT11]);
        if((int32_t)(now - app->dht_next) >= 0) {
            float temperature, humidity;
            const bool ok = dht11_read(dht->pin, &temperature, &humidity);
            if(ok) {
                app->dht_temp = temperature;
                app->dht_humidity = humidity;
                new_data = true;
            }
            app->dht_valid = ok;
            app->dht_attempted = true;
            app->dht_next = now + DHT11_PERIOD_MS;
        }
    }

    if(app->ch_voltage) {
        if((int32_t)(now - app->voltage_next) >= 0) {
            app->voltage = voltage_read(app);
            app->voltage_attempted = true;
            app->voltage_next = now + VOLTAGE_PERIOD_MS;
            new_data = true;
        }
    }

    bool ready = true;
    if(app->ch_ds && !app->ds_attempted) ready = false;
    if(app->ch_dht_temp && !app->dht_attempted) ready = false;
    if(app->ch_voltage && !app->voltage_attempted) ready = false;

    if(ready && (new_data || !app->notifying)) {
        /* Wait until the phone subscribes: a notification before that is dropped. */
        if(app->server->subscribed()) {
            float values[PHYPHOX_VALUES] = {0};
            if(app->ch_ds) values[app->ch_ds - 1] = app->ds_temp;
            if(app->ch_dht_temp) values[app->ch_dht_temp - 1] = app->dht_temp;
            if(app->ch_dht_humidity) values[app->ch_dht_humidity - 1] = app->dht_humidity;
            if(app->ch_voltage) values[app->ch_voltage - 1] = app->voltage;
            app->server->writeFloats(values, PHYPHOX_VALUES);
            app->notifying = true;
            view_port_update(app->viewport);
        } else if(new_data) {
            view_port_update(app->viewport);
        }
    } else if(new_data) {
        view_port_update(app->viewport);
    }
}

static bool stream_start(AppContext* app) {
    const GpioPinRecord* ds = pin_record(app->config_pin[SensorDS18B20]);
    const GpioPinRecord* dht = pin_record(app->config_pin[SensorDHT11]);
    const GpioPinRecord* voltage_record = pin_record(app->config_pin[SensorVoltage]);
    if(!ds && !dht && !voltage_record) return false;

    stream_stop(app);

    if(ds) {
        app->onewire = onewire_host_alloc(ds->pin);
        furi_check(app->onewire);
        onewire_host_start(app->onewire);
    }
    if(dht) {
        furi_hal_gpio_init(dht->pin, GpioModeInput, GpioPullUp, GpioSpeedLow);
    }
    if(voltage_record) {
        voltage_start(app, voltage_record);
    }

    power_enable_otg(app->power, true);
    app->otg_enabled = true;

    const uint32_t now = phyphox_millis();
    app->dht_next = now;
    app->voltage_next = now;
    app->notifying = false;

    app->streaming = true;
    view_port_update(app->viewport);
    return true;
}

/* ---------------------------------------------------------------- drawing */

static const char* ble_status(const FlipperTransport* transport) {
    if(transport->has_error()) return "BLE: error";
    if(transport->connected()) return "BLE: connected";
    if(transport->is_advertising()) return "BLE: advertising";
    return "BLE: starting";
}

static void format_tenths(char* out, size_t size, float value) {
    const int32_t tenths = (int32_t)(value * 10.0f + (value >= 0 ? 0.5f : -0.5f));
    if(tenths < 0) {
        snprintf(out, size, "-%ld.%ld", (long)(-tenths / 10), (long)(-tenths % 10));
    } else {
        snprintf(out, size, "%ld.%ld", (long)(tenths / 10), (long)(tenths % 10));
    }
}

static void draw_selected_row(Canvas* canvas, uint8_t y, uint8_t height, const char* text) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, y, 128, height);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str_aligned(canvas, 3, y + 1, AlignLeft, AlignTop, text);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_footer(Canvas* canvas, const char* text) {
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, text);
}

static void draw_main(Canvas* canvas, AppContext* app) {
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < 4; i++) {
        const uint8_t y = 15 + i * 9;
        char text[24];
        if(i < SensorTypeCount) {
            if(app->config_pin[i]) {
                snprintf(text, sizeof(text), "%-7s pin %u", kSensorNames[i], app->config_pin[i]);
            } else {
                snprintf(text, sizeof(text), "%-7s off", kSensorNames[i]);
            }
        } else {
            strlcpy(text, "Start streaming", sizeof(text));
        }
        if(i == app->selected) {
            draw_selected_row(canvas, y, 9, text);
        } else {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_str_aligned(canvas, 3, y + 1, AlignLeft, AlignTop, text);
        }
    }
}

static void draw_pins(Canvas* canvas, AppContext* app) {
    canvas_set_font(canvas, FontSecondary);
    const uint8_t first = app->pin_selected > 3 ? app->pin_selected - 3 : 0;
    for(uint8_t row = 0; row < 4; row++) {
        const uint8_t index = first + row;
        if(index >= app->pin_count) break;
        const uint8_t y = 15 + row * 9;
        char text[24];
        if(app->pin_options[index] == 0) {
            strlcpy(text, "Off", sizeof(text));
        } else {
            const GpioPinRecord* record = pin_record(app->pin_options[index]);
            snprintf(text, sizeof(text), "pin %u  %s", record->number, record->name);
        }
        if(index == app->pin_selected) {
            draw_selected_row(canvas, y, 9, text);
        } else {
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_str_aligned(canvas, 3, y + 1, AlignLeft, AlignTop, text);
        }
    }
}

static void draw_stream(Canvas* canvas, AppContext* app) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 17, AlignCenter, AlignTop, ble_status(app->transport));

    uint8_t y = 28;
    char text[28], value[12];

    if(app->ch_ds) {
        if(!app->ds_attempted) {
            strlcpy(text, "DS18B20  measuring...", sizeof(text));
        } else if(!app->ds_valid) {
            strlcpy(text, "DS18B20  no sensor", sizeof(text));
        } else {
            format_tenths(value, sizeof(value), app->ds_temp);
            snprintf(text, sizeof(text), "DS18B20  %s C", value);
        }
        canvas_draw_str_aligned(canvas, 3, y, AlignLeft, AlignTop, text);
        y += 9;
    }

    if(app->ch_dht_temp) {
        if(!app->dht_attempted) {
            strlcpy(text, "DHT11    measuring...", sizeof(text));
        } else if(!app->dht_valid) {
            strlcpy(text, "DHT11    no sensor", sizeof(text));
        } else {
            format_tenths(value, sizeof(value), app->dht_temp);
            snprintf(
                text,
                sizeof(text),
                "DHT11  %s C %d %%",
                value,
                (int)(app->dht_humidity + 0.5f));
        }
        canvas_draw_str_aligned(canvas, 3, y, AlignLeft, AlignTop, text);
        y += 9;
    }

    if(app->ch_voltage) {
        if(!app->voltage_attempted) {
            strlcpy(text, "Voltage  measuring...", sizeof(text));
        } else {
            const int32_t millivolts = (int32_t)(app->voltage * 1000.0f + 0.5f);
            snprintf(
                text,
                sizeof(text),
                "Voltage  %ld.%03ld V",
                (long)(millivolts / 1000),
                (long)(millivolts % 1000));
        }
        canvas_draw_str_aligned(canvas, 3, y, AlignLeft, AlignTop, text);
    }
}

extern "C" void phyphox_draw(Canvas* canvas, void* context) {
    AppContext* app = (AppContext*)context;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    char title[24];
    if(app->ui_mode == UiPinPick && !app->streaming) {
        snprintf(title, sizeof(title), "%s pin", kSensorNames[app->pin_type]);
    } else {
        strlcpy(title, "Phyphox Companion", sizeof(title));
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 11, AlignCenter, AlignBottom, title);
    canvas_draw_line(canvas, 0, 13, 127, 13);

    if(app->streaming) {
        draw_stream(canvas, app);
        draw_footer(canvas, "Back: stop");
        return;
    }

    if(app->ui_mode == UiPinPick) {
        draw_pins(canvas, app);
        draw_footer(canvas, "OK choose   Back");
    } else {
        draw_main(canvas, app);
        draw_footer(
            canvas,
            app->message[0] ? app->message : "OK select   Back exit");
    }
}

/* ---------------------------------------------------------------- input */

static void open_pin_picker(AppContext* app, SensorType type) {
    app->pin_type = type;
    app->pin_count = 0;
    app->pin_options[app->pin_count++] = 0; // Off

    for(size_t i = 0; i < gpio_pins_count && app->pin_count < MAX_PIN_OPTIONS; i++) {
        const GpioPinRecord* record = &gpio_pins[i];
        if(record->number == 0) continue; // not on the external header
        if(type == SensorVoltage && record->channel == FuriHalAdcChannelNone) continue;
        bool used = false;
        for(uint8_t other = 0; other < SensorTypeCount; other++) {
            if(other != type && app->config_pin[other] == record->number) used = true;
        }
        if(!used) app->pin_options[app->pin_count++] = record->number;
    }

    app->pin_selected = 0;
    for(uint8_t i = 1; i < app->pin_count; i++) {
        if(app->pin_options[i] == app->config_pin[type]) app->pin_selected = i;
    }
    app->ui_mode = UiPinPick;
}

static bool handle_input(AppContext* app, InputEvent* event) {
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return true;
    app->message[0] = '\0';

    if(app->streaming) {
        if(event->type == InputTypeShort && event->key == InputKeyBack) {
            stream_stop(app);
            view_port_update(app->viewport);
        }
        return true;
    }

    if(app->ui_mode == UiPinPick) {
        const uint8_t last = app->pin_count - 1;
        if(event->key == InputKeyUp) {
            app->pin_selected = app->pin_selected ? app->pin_selected - 1 : last;
        } else if(event->key == InputKeyDown) {
            app->pin_selected = app->pin_selected == last ? 0 : app->pin_selected + 1;
        } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
            app->config_pin[app->pin_type] = app->pin_options[app->pin_selected];
            app->ui_mode = UiMain;

            compute_channels(app);
            app->transport->disconnect();
            server_update_experiment(app);
        } else if(event->type == InputTypeShort && event->key == InputKeyBack) {
            app->ui_mode = UiMain;
        }
        view_port_update(app->viewport);
        return true;
    }

    if(event->key == InputKeyUp) {
        app->selected = app->selected ? app->selected - 1 : 3;
    } else if(event->key == InputKeyDown) {
        app->selected = (uint8_t)((app->selected + 1) % 4);
    } else if(event->type == InputTypeShort && event->key == InputKeyOk) {
        if(app->selected < SensorTypeCount) {
            open_pin_picker(app, (SensorType)app->selected);
        } else if(
            app->config_pin[SensorDS18B20] || app->config_pin[SensorDHT11] ||
            app->config_pin[SensorVoltage]) {
            if(!stream_start(app)) {
                strlcpy(app->message, "Sensor start failed", sizeof(app->message));
            }
        } else {
            strlcpy(app->message, "Select a sensor first", sizeof(app->message));
        }
    } else if(event->type == InputTypeShort && event->key == InputKeyBack) {
        return false;
    }

    view_port_update(app->viewport);
    return true;
}

extern "C" void phyphox_input(InputEvent* event, void* context) {
    AppContext* app = (AppContext*)context;
    furi_message_queue_put(app->queue, event, 0);
}

/* ---------------------------------------------------------------- app lifecycle */

static AppContext* app_alloc(void) {
    AppContext* app = (AppContext*)malloc(sizeof(AppContext));
    furi_check(app);
    memset(app, 0, sizeof(*app));

    void* transport_memory = malloc(sizeof(FlipperTransport));
    furi_check(transport_memory);
    app->transport = new(transport_memory) FlipperTransport();

    app->server_memory = malloc(sizeof(phyphox::Server));
    furi_check(app->server_memory);

    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(app->queue);

    app->viewport = view_port_alloc();
    furi_check(app->viewport);
    view_port_draw_callback_set(app->viewport, phyphox_draw, app);
    view_port_input_callback_set(app->viewport, phyphox_input, app);

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->viewport, GuiLayerFullscreen);

    app->power = (Power*)furi_record_open(RECORD_POWER);
    furi_check(app->power);

    /* Keep the old app's behaviour by default: DS18B20 on the 1-Wire pin. */
    app->config_pin[SensorDS18B20] = 17;

    /* phyphox expects the service to exist whenever it reconnects, so start
     * the profile now and leave it running while the menu is shown. */
    server_start(app);
    if(app->transport->has_error()) {
        strlcpy(app->message, "BLE error", sizeof(app->message));
    }
    view_port_update(app->viewport);

    return app;
}

static void app_free(AppContext* app) {
    stream_stop(app);
    server_stop(app);

    view_port_enabled_set(app->viewport, false);
    gui_remove_view_port(app->gui, app->viewport);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_POWER);

    furi_message_queue_free(app->queue);
    view_port_free(app->viewport);

    app->transport->~FlipperTransport();
    free(app->transport);
    free(app->server_memory);
    free(app);
}

extern "C" int32_t phyphox_app_main(void* p) {
    (void)p;

    AppContext* app = app_alloc();
    InputEvent event;

    while(true) {
        if(app->streaming) {
            const FuriStatus status = furi_message_queue_get(app->queue, &event, UI_TICK_MS);
            if(status == FuriStatusOk && !handle_input(app, &event)) break;
            if(app->streaming) stream_tick(app);
        } else {
            /* Keep the phyphox transfer serviced while the menu is shown: the
             * phone may connect and download the experiment before Start. */
            const FuriStatus status = furi_message_queue_get(app->queue, &event, UI_TICK_MS);
            if(status == FuriStatusOk && !handle_input(app, &event)) break;
            app->server->poll();
        }
    }

    app_free(app);
    return 0;
}
