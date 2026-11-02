#include <furi.h>
#include <furi_hal_resources.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <core/kernel.h>
#include <core/thread.h>
#include <power/power_service/power.h>
#include <one_wire/maxim_crc.h>
#include <one_wire/one_wire_host.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phyphox_ble.h"
#include "lib/phyphox/core/Server.h"
#include "lib/phyphox/phyphoxBleExperiment.h"

#define WORKER_STACK_SIZE (6 * 1024)
#define SAMPLE_PERIOD_MS  1000

#define DS18B20_CMD_SKIP_ROM        0xccU
#define DS18B20_CMD_CONVERT         0x44U
#define DS18B20_CMD_READ_SCRATCHPAD 0xbeU

#define DS18B20_CFG_RESOLUTION_POS  5U
#define DS18B20_CFG_RESOLUTION_MASK 0x03U
#define DS18B20_DECIMAL_PART_MASK   0x0fU
#define DS18B20_SIGN_MASK           0xf0U

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

typedef struct {
    Gui* gui;
    ViewPort* viewport;
    FuriMessageQueue* queue;
    FuriThread* worker;
    FlipperTransport* transport;
    volatile bool running;
    volatile bool has_device;
    volatile bool has_reading;
    volatile float temp_celsius;
} AppContext;

static uint32_t phyphox_millis(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency());
}

/* The DS18B20 driver follows the official Flipper example_thermo app. */
static void ds18b20_request(AppContext* app, OneWireHost* onewire) {
    FURI_CRITICAL_ENTER();

    bool success = false;
    do {
        if(!onewire_host_reset(onewire)) break;
        onewire_host_write(onewire, DS18B20_CMD_SKIP_ROM);
        onewire_host_write(onewire, DS18B20_CMD_CONVERT);
        success = true;
    } while(false);

    FURI_CRITICAL_EXIT();

    app->has_device = success;
    if(!success) app->has_reading = false;
}

static bool ds18b20_read(AppContext* app, OneWireHost* onewire) {
    if(!app->has_device) return false;

    FURI_CRITICAL_ENTER();

    bool success = false;
    do {
        DS18B20Scratchpad buf;
        size_t attempts_left = 10;

        do {
            if(!onewire_host_reset(onewire)) continue;
            onewire_host_write(onewire, DS18B20_CMD_SKIP_ROM);
            onewire_host_write(onewire, DS18B20_CMD_READ_SCRATCHPAD);
            onewire_host_read_bytes(onewire, buf.bytes, sizeof(buf.bytes));
            const uint8_t crc = maxim_crc8(buf.bytes, sizeof(buf.bytes) - 1, MAXIM_CRC8_INIT);
            if(crc == buf.fields.crc) break;
        } while(--attempts_left);

        if(attempts_left == 0) break;

        const uint8_t resolution =
            (buf.fields.config >> DS18B20_CFG_RESOLUTION_POS) & DS18B20_CFG_RESOLUTION_MASK;
        const uint8_t decimal_mask =
            (DS18B20_DECIMAL_PART_MASK << (DS18B20_CFG_RESOLUTION_MASK - resolution)) &
            DS18B20_DECIMAL_PART_MASK;
        const uint8_t integer_part =
            (buf.fields.temp_msb << 4U) | (buf.fields.temp_lsb >> 4U);
        const uint8_t decimal_part = buf.fields.temp_lsb & decimal_mask;
        const bool negative = (buf.fields.temp_msb & DS18B20_SIGN_MASK) != 0;
        const float abs_celsius = integer_part + decimal_part / 16.0f;

        app->temp_celsius = negative ? -abs_celsius : abs_celsius;
        success = true;
    } while(false);

    FURI_CRITICAL_EXIT();

    app->has_device = success;
    app->has_reading = success;
    return success;
}

extern "C" int32_t phyphox_worker(void* context) {
    AppContext* app = (AppContext*)context;

    phyphox::Server server(*app->transport);
    server.setClock(phyphox_millis);
    server.setDeviceName("FLIP");

    {
        PhyphoxBleExperiment experiment(
            "DS18B20 Temperature",
            "Flipper Zero",
            "Temperature from a DS18B20 1-Wire sensor on GPIO pin 17.");

        PhyphoxBleExperiment::View view("Temperature");

        PhyphoxBleExperiment::Graph graph("Temperature over time");
        graph.setChannel(0, 1);
        graph.setLabelX("Time");
        graph.setUnitX("s");
        graph.setLabelY("Temperature");
        graph.setUnitY("C");

        PhyphoxBleExperiment::Value value("Temperature", 1);
        value.setUnit("C");
        value.setPrecision(1);

        view.addElement(graph);
        view.addElement(value);
        experiment.addView(view);

        server.addExperiment(experiment);
    }
    server.start();

    Power* power = (Power*)furi_record_open(RECORD_POWER);
    power_enable_otg(power, true);

    OneWireHost* onewire = onewire_host_alloc(&gpio_ibutton);
    onewire_host_start(onewire);

    bool converting = false;
    uint32_t conversion_started = 0;

    while(app->running) {
        server.poll();

        const uint32_t now = phyphox_millis();
        if(!converting) {
            ds18b20_request(app, onewire);
            converting = true;
            conversion_started = now;
            view_port_update(app->viewport);
        } else if((uint32_t)(now - conversion_started) >= SAMPLE_PERIOD_MS) {
            if(ds18b20_read(app, onewire) && app->transport->connected()) {
                float temperature = app->temp_celsius;
                server.writeFloats(&temperature, 1);
            }
            converting = false;
            view_port_update(app->viewport);
        }

        furi_delay_ms(10);
    }

    onewire_host_stop(onewire);
    onewire_host_free(onewire);
    power_enable_otg(power, false);
    furi_record_close(RECORD_POWER);

    app->transport->stop();
    return 0;
}

extern "C" void phyphox_draw(Canvas* canvas, void* context) {
    AppContext* app = (AppContext*)context;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 11, AlignCenter, AlignBottom, "F0 Phyphox");
    canvas_draw_line(canvas, 0, 14, 127, 14);

    canvas_set_font(canvas, FontSecondary);
    const char* status;
    if(app->transport->has_error()) {
        status = "BLE: error";
    } else if(app->transport->connected()) {
        status = "BLE: connected";
    } else if(app->transport->is_advertising()) {
        status = "BLE: advertising";
    } else {
        status = "BLE: starting";
    }
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignBottom, status);

    canvas_set_font(canvas, FontKeyboard);
    char text[24];
    if(app->has_reading) {
        // Integer formatting, so the app does not depend on newlib float printf.
        const float temperature = app->temp_celsius;
        int32_t tenths = (int32_t)(temperature * 10.0f + (temperature >= 0 ? 0.5f : -0.5f));
        if(tenths < 0) {
            snprintf(text, sizeof(text), "-%ld.%ld C", (long)(-tenths / 10), (long)(-tenths % 10));
        } else {
            snprintf(text, sizeof(text), "+%ld.%ld C", (long)(tenths / 10), (long)(tenths % 10));
        }
    } else if(app->has_device) {
        strlcpy(text, "measuring...", sizeof(text));
    } else {
        strlcpy(text, "no sensor", sizeof(text));
    }
    canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignBottom, text);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 60, AlignCenter, AlignBottom, "DQ:17  VDD:9  GND:18");
}

extern "C" void phyphox_input(InputEvent* event, void* context) {
    AppContext* app = (AppContext*)context;
    furi_message_queue_put(app->queue, event, 0);
}

static AppContext* app_alloc(void) {
    AppContext* app = (AppContext*)malloc(sizeof(AppContext));
    furi_check(app);
    memset(app, 0, sizeof(*app));
    app->running = true;

    void* transport_memory = malloc(sizeof(FlipperTransport));
    furi_check(transport_memory);
    app->transport = new(transport_memory) FlipperTransport();

    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    furi_check(app->queue);

    app->viewport = view_port_alloc();
    furi_check(app->viewport);
    view_port_draw_callback_set(app->viewport, phyphox_draw, app);
    view_port_input_callback_set(app->viewport, phyphox_input, app);

    app->worker = furi_thread_alloc_ex(
        "PhyphoxWorker", WORKER_STACK_SIZE, phyphox_worker, app);
    furi_check(app->worker);

    app->gui = (Gui*)furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->viewport, GuiLayerFullscreen);

    return app;
}

static void app_free(AppContext* app) {
    view_port_enabled_set(app->viewport, false);
    gui_remove_view_port(app->gui, app->viewport);
    furi_record_close(RECORD_GUI);

    furi_thread_free(app->worker);
    furi_message_queue_free(app->queue);
    view_port_free(app->viewport);

    app->transport->~FlipperTransport();
    free(app->transport);
    free(app);
}

extern "C" int32_t phyphox_app_main(void* p) {
    (void)p;

    AppContext* app = app_alloc();
    furi_thread_start(app->worker);

    InputEvent event;
    while(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyBack) break;
    }

    app->running = false;
    furi_thread_join(app->worker);
    app_free(app);
    return 0;
}
