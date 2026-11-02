#include "phyphox_ble.h"

#include <furi.h>
#include <furi_hal_version.h>
#include <bt/bt_service/bt.h>
#include <furi_ble/gatt.h>
#include <furi_ble/event_dispatcher.h>

extern "C" {
#include <ble/core/ble_defs.h>
#include <ble/core/ble_std.h>
#include <ble/core/auto/ble_types.h>
}

/*
 * The SDK exports the GATT/event-dispatcher API but not ST's ble/ble.h
 * wrapper, so the three HCI packet headers from that file are repeated here.
 */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t data[1];
} hci_uart_pckt;

typedef struct __attribute__((packed)) {
    uint8_t evt;
    uint8_t plen;
    uint8_t data[1];
} hci_event_pckt;

typedef struct __attribute__((packed)) {
    uint16_t ecode;
    uint8_t data[1];
} evt_blecore_aci;

/* From ST's ble_events.h; the SDK does not ship that header. The value was
 * confirmed against the firmware's own serial service (it compares 0x0C01). */
#define ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE 0x0C01

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PhyphoxBle"

/*
 * phyphox BLE contract. 128-bit UUIDs are stored in Bluetooth little-endian
 * byte order, i.e. the reverse of the "cddf....-30f7-4671-8b43-5e40ba53514a"
 * spelling used in the experiment XML.
 */
static const uint8_t kExperimentServiceUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x01, 0x00, 0xdf, 0xcd,
};
static const uint8_t kDataServiceUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x01, 0x10, 0xdf, 0xcd,
};
static const uint8_t kExperimentCharUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x02, 0x00, 0xdf, 0xcd,
};
static const uint8_t kControlCharUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x03, 0x00, 0xdf, 0xcd,
};
static const uint8_t kEventCharUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x04, 0x00, 0xdf, 0xcd,
};
static const uint8_t kDataCharUuid[16] = {
    0x4a, 0x51, 0x53, 0xba, 0x40, 0x5e, 0x43, 0x8b,
    0x71, 0x46, 0xf7, 0x30, 0x02, 0x10, 0xdf, 0xcd,
};

/* ATT MTU 23 - 3. The phyphox library chunks the experiment at this size. */
#define PHYPHOX_VALUE_LEN 20

/*
 * The ST stack adds BLE flags (3) and TX power (3) itself. The 128-bit
 * service UUID AD element is 18 bytes, so the local-name AD element
 * (length + type + chars) has 7 bytes left: 5 characters maximum. Use 4
 * for one byte of margin, or the stack rejects the whole advertising set.
 */
#define PHYPHOX_NAME_MAX 7

struct NotifyCtx {
    const uint8_t* data;
    uint16_t len;
};

struct PhyphoxProfile;

struct PhyphoxBleState {
    Bt* bt;
    PhyphoxProfile* profile;
    phyphox::TransportListener* listener;
    volatile bool connected;
    volatile bool advertising;
    volatile bool error;
    char device_name[PHYPHOX_NAME_MAX + 1];
};

struct PhyphoxProfile {
    FuriHalBleProfileBase base;
    PhyphoxBleState* state;
    BleGattCharacteristicInstance experiment_char;
    BleGattCharacteristicInstance control_char;
    BleGattCharacteristicInstance event_char;
    BleGattCharacteristicInstance data_char;
    uint16_t experiment_service;
    uint16_t data_service;
    GapSvcEventHandler* event_handler;
};

static_assert(offsetof(PhyphoxProfile, base) == 0, "Wrong profile layout");

extern "C" bool phyphox_char_data(
    const void* context,
    const uint8_t** data,
    uint16_t* data_len) {
    if(data == NULL) {
        *data_len = PHYPHOX_VALUE_LEN; // characteristic max length at init
        return false;
    }
    const NotifyCtx* ctx = (const NotifyCtx*)context;
    *data = ctx->data;
    *data_len = ctx->len;
    return false;
}

static BleGattCharacteristicParams make_characteristic(
    const char* name,
    const uint8_t uuid[16],
    uint8_t properties,
    bool notify) {
    BleGattCharacteristicParams p = {};
    p.name = name;
    p.uuid_type = UUID_TYPE_128;
    memcpy(p.uuid.Char_UUID_128, uuid, sizeof(p.uuid.Char_UUID_128));
    p.char_properties = properties;
    p.security_permissions = ATTR_PERMISSION_NONE;
    p.gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE;
    p.is_variable = CHAR_VALUE_LEN_VARIABLE;
    if(notify) {
        p.data_prop_type = FlipperGattCharacteristicDataCallback;
        p.data.callback.fn = phyphox_char_data;
        p.data.callback.context = NULL;
    } else {
        p.data_prop_type = FlipperGattCharacteristicDataFixed;
        p.data.fixed.ptr = NULL;
        p.data.fixed.length = PHYPHOX_VALUE_LEN;
    }
    return p;
}

/* Writes and CCCD changes arrive here, in the BLE event thread. */
extern "C" BleEventAckStatus phyphox_gatt_event(void* event, void* context) {
    PhyphoxProfile* profile = (PhyphoxProfile*)context;
    PhyphoxBleState* state = profile->state;

    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    if(event_pckt->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) return BleEventNotAck;

    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    if(blecore_evt->ecode != ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) return BleEventNotAck;

    aci_gatt_attribute_modified_event_rp0* modified =
        (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;
    uint16_t handle = modified->Attr_Handle;

    if(handle == profile->experiment_char.handle + 2) {
        bool enabled = modified->Attr_Data_Length >= 2 && (modified->Attr_Data[0] & 0x01);
        if(state->listener) {
            state->listener->onSubscribe(phyphox::CharId{phyphox::CH_EXPERIMENT, 0}, enabled);
        }
        return BleEventAckFlowEnable;
    }
    if(handle == profile->data_char.handle + 2) {
        bool enabled = modified->Attr_Data_Length >= 2 && (modified->Attr_Data[0] & 0x01);
        if(state->listener) {
            state->listener->onSubscribe(phyphox::CharId{phyphox::CH_DATA, 0}, enabled);
        }
        return BleEventAckFlowEnable;
    }
    if(handle == profile->control_char.handle + 1) {
        if(state->listener) {
            state->listener->onWrite(
                phyphox::CharId{phyphox::CH_CONTROL, 0},
                modified->Attr_Data,
                modified->Attr_Data_Length);
        }
        return BleEventAckFlowEnable;
    }
    if(handle == profile->event_char.handle + 1) {
        if(state->listener) {
            state->listener->onWrite(
                phyphox::CharId{phyphox::CH_EVENT, 0},
                modified->Attr_Data,
                modified->Attr_Data_Length);
        }
        return BleEventAckFlowEnable;
    }
    return BleEventNotAck;
}

extern "C" void phyphox_bt_status(BtStatus status, void* context) {
    PhyphoxBleState* state = (PhyphoxBleState*)context;
    bool was_connected = state->connected;
    state->connected = (status == BtStatusConnected);
    state->advertising = (status == BtStatusAdvertising);
    FURI_LOG_I(TAG, "BT status %d", (int)status);

    if(state->listener && state->connected && !was_connected) {
        state->listener->onConnect();
    } else if(state->listener && !state->connected && was_connected) {
        state->listener->onDisconnect();
    }
}

extern "C" FuriHalBleProfileBase* phyphox_profile_start(FuriHalBleProfileParams params);
extern "C" void phyphox_profile_stop(FuriHalBleProfileBase* profile);
extern "C" void phyphox_profile_get_gap_config(
    GapConfig* config,
    FuriHalBleProfileParams params);

static const FuriHalBleProfileTemplate phyphox_profile_template = {
    .start = phyphox_profile_start,
    .stop = phyphox_profile_stop,
    .get_gap_config = phyphox_profile_get_gap_config,
};

extern "C" FuriHalBleProfileBase* phyphox_profile_start(FuriHalBleProfileParams params) {
    PhyphoxBleState* state = (PhyphoxBleState*)params;

    PhyphoxProfile* profile = (PhyphoxProfile*)malloc(sizeof(PhyphoxProfile));
    if(!profile) return NULL;
    memset(profile, 0, sizeof(*profile));
    profile->base.config = &phyphox_profile_template;
    profile->state = state;

    Service_UUID_t service = {};
    memcpy(service.Service_UUID_128, kExperimentServiceUuid, sizeof(kExperimentServiceUuid));
    if(!ble_gatt_service_add(
           UUID_TYPE_128, &service, PRIMARY_SERVICE, 12, &profile->experiment_service)) {
        FURI_LOG_E(TAG, "Experiment service add failed");
        free(profile);
        return NULL;
    }

    BleGattCharacteristicParams characteristic = make_characteristic(
        "Experiment", kExperimentCharUuid, CHAR_PROP_READ | CHAR_PROP_NOTIFY, true);
    ble_gatt_characteristic_init(
        profile->experiment_service, &characteristic, &profile->experiment_char);

    characteristic = make_characteristic(
        "Control",
        kControlCharUuid,
        CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
        false);
    ble_gatt_characteristic_init(
        profile->experiment_service, &characteristic, &profile->control_char);

    characteristic = make_characteristic(
        "Event",
        kEventCharUuid,
        CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
        false);
    ble_gatt_characteristic_init(
        profile->experiment_service, &characteristic, &profile->event_char);

    memcpy(service.Service_UUID_128, kDataServiceUuid, sizeof(kDataServiceUuid));
    if(!ble_gatt_service_add(UUID_TYPE_128, &service, PRIMARY_SERVICE, 8, &profile->data_service)) {
        FURI_LOG_E(TAG, "Data service add failed");
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->experiment_char);
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->control_char);
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->event_char);
        ble_gatt_service_delete(profile->experiment_service);
        free(profile);
        return NULL;
    }

    characteristic = make_characteristic(
        "Data", kDataCharUuid, CHAR_PROP_READ | CHAR_PROP_NOTIFY, true);
    ble_gatt_characteristic_init(profile->data_service, &characteristic, &profile->data_char);

    profile->event_handler = ble_event_dispatcher_register_svc_handler(phyphox_gatt_event, profile);
    state->profile = profile;
    FURI_LOG_I(TAG, "Profile started");
    return &profile->base;
}

extern "C" void phyphox_profile_stop(FuriHalBleProfileBase* base) {
    if(!base || base->config != &phyphox_profile_template) return;
    PhyphoxProfile* profile = (PhyphoxProfile*)base;

    if(profile->event_handler) {
        ble_event_dispatcher_unregister_svc_handler(profile->event_handler);
    }
    if(profile->experiment_service) {
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->experiment_char);
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->control_char);
        ble_gatt_characteristic_delete(profile->experiment_service, &profile->event_char);
        ble_gatt_service_delete(profile->experiment_service);
    }
    if(profile->data_service) {
        ble_gatt_characteristic_delete(profile->data_service, &profile->data_char);
        ble_gatt_service_delete(profile->data_service);
    }
    if(profile->state) profile->state->profile = NULL;
    free(profile);
}

extern "C" void phyphox_profile_get_gap_config(
    GapConfig* config,
    FuriHalBleProfileParams params) {
    PhyphoxBleState* state = (PhyphoxBleState*)params;
    memset(config, 0, sizeof(*config));

    config->adv_service.UUID_Type = UUID_TYPE_128;
    memcpy(
        config->adv_service.Service_UUID_128,
        kExperimentServiceUuid,
        sizeof(kExperimentServiceUuid));

    config->appearance_char = 0;
    config->bonding_mode = false;
    config->pairing_method = GapPairingNone;
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));

    // gap_init_svc() skips byte 0 when it registers the GAP device name.
    config->adv_name[0] = AD_TYPE_COMPLETE_LOCAL_NAME;
    strlcpy(
        &config->adv_name[1],
        state->device_name[0] ? state->device_name : "FLIP",
        sizeof(config->adv_name) - 1);

    config->conn_param.conn_int_min = 12; // 15 ms
    config->conn_param.conn_int_max = 24; // 30 ms
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 500; // 5 s

    if(strlen(state->device_name) > 5) {
        FURI_LOG_E(TAG, "Name too long for the 128-bit UUID ad packet");
    }
}

FlipperTransport::FlipperTransport() {
    state = (PhyphoxBleState*)malloc(sizeof(PhyphoxBleState));
    furi_check(state);
    memset(state, 0, sizeof(*state));
}

FlipperTransport::~FlipperTransport() {
    stop();
    free(state);
    state = NULL;
}

void FlipperTransport::stop() {
    if(!state) return;
    state->listener = NULL;
    if(state->bt) {
        bt_set_status_changed_callback(state->bt, NULL, NULL);
        bt_profile_restore_default(state->bt);
        furi_record_close(RECORD_BT);
        state->bt = NULL;
    }
    state->profile = NULL;
    state->connected = false;
    state->advertising = false;
}

bool FlipperTransport::begin(
    const phyphox::GattLayout& layout,
    phyphox::TransportListener& listener) {
    if(!state || state->bt) return false;

    strlcpy(
        state->device_name,
        layout.deviceName ? layout.deviceName : "FLIP",
        sizeof(state->device_name));
    state->listener = &listener;
    state->error = false;

    state->bt = (Bt*)furi_record_open(RECORD_BT);
    if(!state->bt) {
        state->listener = NULL;
        state->error = true;
        return false;
    }

    bt_set_status_changed_callback(state->bt, phyphox_bt_status, state);
    FuriHalBleProfileBase* profile =
        bt_profile_start(state->bt, &phyphox_profile_template, state);
    if(!profile) {
        bt_set_status_changed_callback(state->bt, NULL, NULL);
        bt_profile_restore_default(state->bt);
        furi_record_close(RECORD_BT);
        state->bt = NULL;
        state->listener = NULL;
        state->error = true;
        return false;
    }

    state->profile = (PhyphoxProfile*)profile;
    return true;
}

bool FlipperTransport::notify(phyphox::CharId id, const uint8_t* data, uint16_t len) {
    if(!state || !state->profile) return false;

    PhyphoxProfile* profile = state->profile;
    uint16_t service;
    BleGattCharacteristicInstance* characteristic;

    switch(id.kind) {
    case phyphox::CH_EXPERIMENT:
        service = profile->experiment_service;
        characteristic = &profile->experiment_char;
        break;
    case phyphox::CH_DATA:
        service = profile->data_service;
        characteristic = &profile->data_char;
        break;
    default:
        return false;
    }

    NotifyCtx ctx = {data, len};
    // ble_gatt_characteristic_update() returns true when the update FAILED.
    bool failed = ble_gatt_characteristic_update(service, characteristic, &ctx);
    return !failed;
}

void FlipperTransport::setValue(phyphox::CharId id, const uint8_t* data, uint16_t len) {
    (void)notify(id, data, len);
}

bool FlipperTransport::connected() const {
    return state && state->connected;
}

bool FlipperTransport::is_advertising() const {
    return state && state->advertising;
}

bool FlipperTransport::has_error() const {
    return state && state->error;
}

uint16_t FlipperTransport::mtuPayload() const {
    return PHYPHOX_VALUE_LEN;
}

void FlipperTransport::poll() {
}

void FlipperTransport::restartAdvertising() {
    // The BT service restarts advertising after a disconnect on its own.
}
