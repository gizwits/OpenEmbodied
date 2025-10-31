/* Configures extended advertsing params*/
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "nimble/ble.h"
#include "esp_peripheral.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "adv.h"
#include "esp_log.h"

static const char *tag = "nimble_adv";
struct ble_instance_cb_register ble_instance_cb[BLE_ADV_INSTANCES];
static uint8_t ext_adv_pattern_1[64] = {0};
static size_t ext_adv_pattern_1_len = 0;
static void ble_multi_perform_gatt_proc(ble_addr_t addr);
static int ble_adv_set_addr(uint16_t instance);
static void ble_multi_adv_print_conn_desc(struct ble_gap_conn_desc *desc);

static int
ble_connectable_ext_cb(uint16_t instance)
{
    ESP_LOGI(tag, "In %s, instance = %d", __func__, instance);
    return 0;
}

static int
ble_scannable_legacy_ext_cb(uint16_t instance)
{
    ESP_LOGI(tag, "In %s, instance = %d", __func__, instance);
    return 0;
}


void ble_set_network_status(bool configured)
{
    if (configured) {
        ext_adv_pattern_1[VERSION_TYPE_INDEX + 1] |= NETWORK_CONFIG;
    } else {
        ext_adv_pattern_1[VERSION_TYPE_INDEX + 1] &= ~NETWORK_CONFIG;
    }
}
esp_err_t ble_gen_adv_data(const char *device_name, uint32_t pk, const uint8_t *mac)
{
    ESP_LOGI(tag, "Setting BLE advertisement data...");
    
    if (!device_name || !mac) {
        ESP_LOGE(tag, "Invalid parameters: device_name or mac is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    size_t name_len = strlen(device_name);
    if (name_len > 29) {
        ESP_LOGE(tag, "Device name too long: %d bytes (max 29)", name_len);
        return ESP_ERR_INVALID_ARG;
    }

    size_t index = 0;

    // 1. Flags (3 bytes)
    ext_adv_pattern_1[index++] = 0x02;  // Length
    ext_adv_pattern_1[index++] = 0x01;  // Type (Flags)
    ext_adv_pattern_1[index++] = 0x06;  // Data (LE General Discoverable Mode, BR/EDR Not Supported)

    // 2. Service UUID (4 bytes)
    ext_adv_pattern_1[index++] = 0x03;  // Length
    ext_adv_pattern_1[index++] = 0x03;  // Type (Complete List of 16-bit Service UUIDs)
    ext_adv_pattern_1[index++] = 0xD0;  // UUID (LSB)
    ext_adv_pattern_1[index++] = 0xAB;  // UUID (MSB)

    // 3. Device Name
    ext_adv_pattern_1[index++] = 3 + 1;
    ext_adv_pattern_1[index++] = 0x09;  // Type (Complete Local Name)
    memcpy(&ext_adv_pattern_1[index], "XPG", 3);
    index += 3;

    // 4. Manufacturer Specific Data (16 bytes)
    ext_adv_pattern_1[index++] = 0x0F;  // Length (15 bytes of data)
    ext_adv_pattern_1[index++] = 0xFF;  // Type (Manufacturer Specific Data)
    ext_adv_pattern_1[index++] = 0x3D;  // Company ID (LSB)
    ext_adv_pattern_1[index++] = 0x00;  // Company ID (MSB)
    
    // Version and Function Mask
    ext_adv_pattern_1[index++] = VERSION_TYPE;
    uint8_t function_mask = BLE_VERSION_4_2 | SUPPORT_OTA | NO_SECURITY_AUTH | ONE_DEVICE_SECRET;
    ext_adv_pattern_1[index++] = function_mask;

    // PK (4 bytes)
    ext_adv_pattern_1[index++] = (pk >> 24) & 0xFF;
    ext_adv_pattern_1[index++] = (pk >> 16) & 0xFF;
    ext_adv_pattern_1[index++] = (pk >> 8) & 0xFF;
    ext_adv_pattern_1[index++] = pk & 0xFF;

    // MAC (6 bytes)
    memcpy(&ext_adv_pattern_1[index], mac, 6);
    index += 6;
    
    ext_adv_pattern_1_len = index;

    // 打印广播数据用于调试
    // ESP_LOGI(tag, "Advertisement data length: %d", ext_adv_pattern_1_len);
    // ESP_LOGI(tag, "Advertisement data:");
    // for (int i = 0; i < ext_adv_pattern_1_len; i++) {
    //     ESP_LOGI(tag, "0x%02X ", ext_adv_pattern_1[i]);
    // }

    return ESP_OK;
}

void gatt_svr_subscribe_cb(struct ble_gap_event *event) {
    /* Check connection handle */
    if (event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(tag, "subscribe event; conn_handle=%d attr_handle=%d",
                event->subscribe.conn_handle, event->subscribe.attr_handle);
    } else {
        ESP_LOGI(tag, "subscribe by nimble stack; attr_handle=%d",
                event->subscribe.attr_handle);
    }

    /* Check attribute handle */
    if (event->subscribe.attr_handle == get_notify_chr_val_handle()) {
        /* Update heart rate subscription status */
        ble_set_notify_handle(event->subscribe.attr_handle);

    }
}

static int
ble_multi_adv_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        MODLOG_DFLT(INFO, "connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            ble_set_conn_handle(event->connect.conn_handle);
            ble_multi_adv_print_conn_desc(&desc);
            ble_multi_perform_gatt_proc(desc.our_id_addr);
        }
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        ble_multi_adv_print_conn_desc(&event->disconnect.conn);
        MODLOG_DFLT(INFO, "\n");
        ble_set_conn_handle(BLE_HS_CONN_HANDLE_NONE);
        ble_multi_advertise(event->disconnect.conn.our_id_addr);

        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        MODLOG_DFLT(INFO, "connection updated; status=%d ",
                    event->conn_update.status);
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        assert(rc == 0);
        ble_multi_adv_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        MODLOG_DFLT(INFO, "encryption change event; status=%d ",
                    event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        assert(rc == 0);
        ble_multi_adv_print_conn_desc(&desc);
        MODLOG_DFLT(INFO, "\n");
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        MODLOG_DFLT(INFO, "notify_tx event; conn_handle=%d attr_handle=%d "
                    "status=%d is_indication=%d",
                    event->notify_tx.conn_handle,
                    event->notify_tx.attr_handle,
                    event->notify_tx.status,
                    event->notify_tx.indication);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO, "subscribe event; conn_handle=%d attr_handle=%d "
                    "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);
        /* GATT subscribe event callback */
        gatt_svr_subscribe_cb(event);
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(tag, "PASSKEY_ACTION_EVENT started");
        struct ble_sm_io pkey = {0};
        int key = 0;

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456; // This is the passkey to be entered on peer
            ESP_LOGI(tag, "Enter passkey %" PRIu32 "on the peer side", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(tag, "Passkey on device's display: %" PRIu32 , event->passkey.params.numcmp);
            ESP_LOGI(tag, "Accept or reject the passkey through console in this format -> key Y or key N");
            pkey.action = event->passkey.params.action;
            if (scli_receive_key(&key)) {
                pkey.numcmp_accept = key;
            } else {
                pkey.numcmp_accept = 0;
                ESP_LOGE(tag, "Timeout! Rejecting the key");
            }
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            static uint8_t tem_oob[16] = {0};
            pkey.action = event->passkey.params.action;
            for (int i = 0; i < 16; i++) {
                pkey.oob[i] = tem_oob[i];
            }
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            ESP_LOGI(tag, "Enter the passkey through console in this format-> key 123456");
            pkey.action = event->passkey.params.action;
            if (scli_receive_key(&key)) {
                pkey.passkey = key;
            } else {
                pkey.passkey = 0;
                ESP_LOGE(tag, "Timeout! Passing 0 as the key");
            }
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_AUTHORIZE:
        MODLOG_DFLT(INFO, "authorize event: conn_handle=%d attr_handle=%d is_read=%d",
                    event->authorize.conn_handle,
                    event->authorize.attr_handle,
                    event->authorize.is_read);

        /* The default behaviour for the event is to reject authorize request */
        event->authorize.out_response = BLE_GAP_AUTHORIZE_REJECT;
        return 0;
    }
    return 0;
}



static void
ble_multi_perform_gatt_proc(ble_addr_t addr)
{
    /* GATT procedures like notify, indicate can be performed now */
    for (int i = 0; i < BLE_ADV_INSTANCES; i++) {
        if (memcmp(&addr, &ble_instance_cb[i].addr, sizeof(addr)) == 0) {
            if (ble_instance_cb[i].cb) {
                ble_instance_cb[i].cb(i);
            }
        }
    }
    return;
}

void
start_connectable_ext(void)
{
    uint8_t instance = 0;
    struct ble_gap_adv_params params;
    int size_pattern = ext_adv_pattern_1_len;

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    params.channel_map = 0;
    params.filter_policy = BLE_HCI_ADV_FILT_DEF;
    params.high_duty_cycle = 0;

    int rc = ble_gap_adv_set_data(ext_adv_pattern_1, size_pattern);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to set advertisement data: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(instance, NULL, BLE_HS_FOREVER, &params, ble_multi_adv_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to start advertising: %d", rc);
        return;
    }

    ESP_LOGI(tag, "Advertising started successfully");
}


void
ble_multi_advertise(ble_addr_t addr)
{
    for (int i = 0; i < BLE_ADV_INSTANCES; i++) {
        if (memcmp(&addr, &ble_instance_cb[i].addr, sizeof(addr)) == 0) {
            switch(i) {
                case 0:
                    start_connectable_ext();
                    break;
                default:
                    ESP_LOGI(tag, "Instance not found");
            }
        }
    }
}

static int ble_adv_set_addr(uint16_t instance)
{
    ble_addr_t addr;
    int rc;

    /* generate new non-resolvable private address */
    rc = ble_hs_id_gen_rnd(1, &addr);
    if (rc != 0) {
        return rc;
    }

    /* Set generated address */
    rc = ble_gap_adv_set_addr(&addr);
    if (rc != 0) {
        return rc;
    }
    memcpy(&ble_instance_cb[instance].addr, &addr, sizeof(addr));
    return 0;
}


static void
ble_multi_adv_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=",
                desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=",
                desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=",
                desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=",
                desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO, " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}
