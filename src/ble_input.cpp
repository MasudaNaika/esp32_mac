/*
 * BLE input for Mac Plus emulator.
 * GATT server with a writable characteristic for mouse/keyboard events.
 *
 * Protocol:
 *   Mouse:    'M' dx:int8 dy:int8 buttons:uint8  (4 bytes)
 *   Key down: 'K' scancode:uint8                  (2 bytes)
 *   Key up:   'U' scancode:uint8                  (2 bytes)
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_input.h"
#include "input_host.h"

static const char *TAG = "ble_input";

static uint8_t s_own_addr_type;
static uint16_t s_input_char_handle;

static const ble_uuid128_t kServiceUuid =
    BLE_UUID128_INIT(0x5f, 0xef, 0xb5, 0xda, 0xcf, 0xc3, 0x37, 0xb6,
                     0x55, 0x4e, 0x58, 0x68, 0x00, 0x01, 0x1e, 0x93);

static const ble_uuid128_t kInputCharUuid =
    BLE_UUID128_INIT(0x5f, 0xef, 0xb5, 0xda, 0xcf, 0xc3, 0x37, 0xb6,
                     0x55, 0x4e, 0x58, 0x68, 0x01, 0x01, 0x1e, 0x93);

static void bleInputAdvertise(void);

// Decode one browser/Web Bluetooth input packet.
// The first byte selects mouse move, key-down, or key-up behavior.
static void handleInputPacket(const uint8_t *data, uint16_t len) {
    if (len < 1) {
        return;
    }

    switch (data[0]) {
    case 'M':
        if (len >= 4) {
            inputHostMouseMove((int8_t)data[1], (int8_t)data[2], data[3] & 1);
        }
        break;
    case 'K':
        if (len >= 2) {
            inputHostKey(data[1], true);
        }
        break;
    case 'U':
        if (len >= 2) {
            inputHostKey(data[1], false);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unknown packet type 0x%02x", data[0]);
        break;
    }
}

static int bleInputAccess(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Writable GATT entry point that converts BLE payloads into emulator input.
    uint8_t buffer[8];
    uint16_t len = 0;
    int rc;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_input_char_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &len);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to decode write payload: rc=%d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    handleInputPacket(buffer, len);
    return 0;
}

static const struct ble_gatt_chr_def kInputCharacteristics[] = {
    {
        .uuid = &kInputCharUuid.u,
        .access_cb = bleInputAccess,
        .arg = NULL,
        .descriptors = NULL,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle = &s_input_char_handle,
        .cpfd = NULL,
    },
    {
        .uuid = NULL,
        .access_cb = NULL,
        .arg = NULL,
        .descriptors = NULL,
        .flags = 0,
        .min_key_size = 0,
        .val_handle = NULL,
        .cpfd = NULL,
    }
};

static const struct ble_gatt_svc_def kGattServices[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .includes = NULL,
        .characteristics = kInputCharacteristics,
    },
    {
        .type = 0,
        .uuid = NULL,
        .includes = NULL,
        .characteristics = NULL,
    }
};

// GAP event handler for the BLE input peripheral.
// The main job is to keep advertising alive across failures and disconnects.
static int bleInputGapEvent(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Client connected");
        } else {
            ESP_LOGW(TAG, "Connect failed: rc=%d", event->connect.status);
            bleInputAdvertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected: reason=%d", event->disconnect.reason);
        inputHostReleaseMouse();
        inputHostReleaseAllKeys();
        bleInputAdvertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising stopped: reason=%d", event->adv_complete.reason);
        bleInputAdvertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

// Start BLE advertising for the custom input service.
// Steps:
// 1. publish flags and service UUID,
// 2. publish the device name as scan response data,
// 3. start undirected general advertising.
static void bleInputAdvertise(void) {
    struct ble_gap_adv_params adv_params = {};
    struct ble_hs_adv_fields fields = {};
    struct ble_hs_adv_fields scan_rsp = {};
    const char *name = ble_svc_gap_device_name();
    ble_uuid128_t advertised_uuid = kServiceUuid;
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &advertised_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising data: rc=%d", rc);
        return;
    }

    scan_rsp.name = (uint8_t *)name;
    scan_rsp.name_len = strlen(name);
    scan_rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan response data: rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleInputGapEvent, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as %s", name);
    }
}

// Log a NimBLE host reset so input failures are visible during bring-up.
static void bleInputOnReset(int reason) {
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
}

// NimBLE sync callback that resolves the local address and starts advertising.
static void bleInputOnSync(void) {
    uint8_t addr_val[6] = {0};
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    bleInputAdvertise();
}

// FreeRTOS task that runs the NimBLE host event loop.
static void bleInputHostTask(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Initialize the BLE input peripheral.
// Steps:
// 1. bring up NimBLE,
// 2. register the custom GATT service and characteristic,
// 3. set the device name and launch the host task.
void bleInputInit() {
    int rc;

    rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE host: rc=%d", rc);
        return;
    }

    ble_hs_cfg.reset_cb = bleInputOnReset;
    ble_hs_cfg.sync_cb = bleInputOnSync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(kGattServices);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(kGattServices);
    assert(rc == 0);

    rc = ble_svc_gap_device_name_set("MacPlus");
    assert(rc == 0);

    nimble_port_freertos_init(bleInputHostTask);
}
