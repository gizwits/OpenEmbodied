/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "config.h"
/* BLE */
#include "esp_crc.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include <esp_wifi.h>

#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "ble.h"
#include "adv.h"


static uint8_t addr_val[6] = {0};
static const char *tag = "NimBLE_BLE_PRPH";
static uint8_t own_addr_type;
static bool is_init = false;

void ble_store_config_init(void);

static struct os_mbuf *
ext_get_data(uint8_t ext_adv_pattern[], int size)
{
    struct os_mbuf *data;
    int rc;
    data = os_msys_get_pkthdr(size, 0);
    assert(data);
    rc = os_mbuf_append(data, ext_adv_pattern, size);
    assert(rc == 0);
    return data;
}

/**
 * Logs information about a connection to the console.
 */
static void
bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
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

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */


static void
bleprph_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

static void
bleprph_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    start_connectable_ext();
}

void bleprph_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}


void ble_init(bool onboarding_flag)
{
    // 强制进配网不判断是否已有ssid
    if(!onboarding_flag)
    {
        // 如果已经存在ssid 和密码则不初始化
        char ssid[32];
        char password[64];
        bool success = storage_load_wifi_config(ssid, password);
        // 并且 ssid 不为空
        if (success && ssid[0] != '\0') {
            ESP_LOGI(tag, "Has wifi infomation in storage, not init ble");
            return;
        }
    }

    ESP_LOGI(tag, "Initializing BLE...");
    int rc;
    esp_err_t ret;

    // 设置广播数据
    ESP_LOGI(tag, "Setting up advertisement data...");
    uint8_t mac[6];
    esp_read_mac(mac, ESP_IF_WIFI_STA);

    char device_name[32];
    snprintf(device_name, sizeof(device_name), "XPG-GAgent-%02X%02X", 
             mac[4], mac[5]);
    
    ESP_LOGI(tag, "Generated device name: %s", device_name);

    // 从storage获取product key
    char pk[33] = {0};
    char did[9] = {0};
    char auth_key[33] = {0};
    char ps[65] = {0};
    uint32_t pk_crc;

    if (storage_get_cached_auth_config(did, auth_key, pk, ps)) {
        // 检查pk是否为空
        if (pk[0] != '\0') {
            pk_crc = esp_crc32_le(0, (const uint8_t*)pk, strlen(pk));
            ESP_LOGI(tag, "Using stored PK: 0x%08X from key: %s", pk_crc, pk);
        } else {
            // pk为空，使用默认值
            const char* default_pk = PRODUCT_KEY;
            pk_crc = esp_crc32_le(0, (const uint8_t*)default_pk, strlen(default_pk));
            ESP_LOGI(tag, "Stored PK empty, using default: 0x%08X from key: %s", pk_crc, default_pk);
        }
    } else {
        // 如果读取失败，使用默认值
        const char* default_pk = PRODUCT_KEY;
        pk_crc = esp_crc32_le(0, (const uint8_t*)default_pk, strlen(default_pk));
        ESP_LOGI(tag, "Failed to get stored PK, using default: 0x%08X from key: %s", pk_crc, default_pk);
    }
    
    ble_gen_adv_data(device_name, pk_crc, mac);

    ESP_LOGI(tag, "Initializing NimBLE port...");
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to init nimble %d", ret);
        return;
    }

    ESP_LOGI(tag, "Configuring BLE host...");
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;

    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc = 0;
    /* Enable the appropriate bit masks to make sure the keys
     * that are needed are exchanged
     */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    ESP_LOGI(tag, "Initializing GATT server...");
    rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to initialize GATT server: %d", rc);
        return;
    }

    ESP_LOGI(tag, "Setting device name...");
    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to set device name: %d", rc);
        return;
    }

    ESP_LOGI(tag, "Initializing BLE store...");
    ble_store_config_init();

    ESP_LOGI(tag, "Starting BLE host task...");
    nimble_port_freertos_init(bleprph_host_task);

    ESP_LOGI(tag, "Initializing command line interface...");
    rc = scli_init();
    if (rc != ESP_OK) {
        ESP_LOGE(tag, "Failed to initialize CLI: %d", rc);
    }

    ESP_LOGI(tag, "BLE initialization completed successfully");
    is_init = true;
}

void
ble_stop(void)
{
    // 检查是否已经初始化
    if (!is_init) {
        ESP_LOGI(tag, "BLE not initialized, nothing to stop");
        return;
    }

    ESP_LOGI(tag, "Stopping BLE...");

    // 停止 BLE 主机任务
    nimble_port_stop();

    // 清理 GATT 服务器
    ble_gatts_reset();

    // 清理 NimBLE 端口
    nimble_port_deinit();

    ESP_LOGI(tag, "BLE stopped successfully");
}

// 保存通知特征值的句柄
static uint16_t notify_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;

// 设置通知句柄的函数
void ble_set_notify_handle(uint16_t handle) {
    notify_handle = handle;
}

// 设置连接句柄的函数
void ble_set_conn_handle(uint16_t handle) {
    conn_handle = handle;
}

bool ble_send_notify(const uint8_t *data, size_t len)
{
    int rc;
    struct os_mbuf *om;

    // 检查参数
    if (!data || len == 0) {
        ESP_LOGE(tag, "Invalid notify data or length");
        return false;
    }

    // 检查连接状态
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGE(tag, "No BLE connection (conn_handle=%d)", conn_handle);
        return false;
    }

    // 检查通知句柄
    if (notify_handle == 0) {
        ESP_LOGE(tag, "Notifications not enabled (notify_handle=%d)", notify_handle);
        return false;
    }

    ESP_LOGI(tag, "Sending notification: conn=%d, handle=%d, len=%d", 
             conn_handle, notify_handle, len);

    // 分配mbuf
    om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGE(tag, "Failed to allocate mbuf");
        return false;
    }

    // 发送通知
    rc = ble_gattc_notify_custom(conn_handle, notify_handle, om);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to send notification: %d (conn=%d, handle=%d)", 
                 rc, conn_handle, notify_handle);
        os_mbuf_free_chain(om);
        return false;
    }

    ESP_LOGI(tag, "Notification sent successfully");
    return true;
}

void ble_stop_advertising(void)
{
    ESP_LOGI(tag, "Stopping BLE advertising...");
    // 停止广播
    ble_gap_adv_stop();
    ESP_LOGI(tag, "BLE advertising stopped");
}

void ble_start_advertising(void)
{
    ESP_LOGI(tag, "Starting BLE advertising...");
    // 重新开始广播
    start_connectable_ext();
    ESP_LOGI(tag, "BLE advertising started");
}


