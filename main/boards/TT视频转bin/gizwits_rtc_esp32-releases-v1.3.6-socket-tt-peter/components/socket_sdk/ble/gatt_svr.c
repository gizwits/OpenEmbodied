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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/ans/ble_svc_ans.h"
#include "protocol/parse_protocol.h"
#include "protocol/pack_protocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdk_api.h"

#define TAG "GATT_SVR"

/*** Maximum number of characteristics with the notify flag ***/
#define MAX_NOTIFY 5

#define GATT_SVR_SVC_CUSTOM_UUID                    0xABD0
#define GATT_SVR_CHR_CUSTOM_READ_UUID              0xABD4
#define GATT_SVR_CHR_CUSTOM_WRITE_UUID             0xABD5
#define GATT_SVR_CHR_CUSTOM_INDICATE_UUID          0xABD6
#define GATT_SVR_CHR_CUSTOM_WRITE_NR_UUID          0xABD7
#define GATT_SVR_CHR_CUSTOM_NOTIFY_UUID            0xABD8

uint16_t notify_chr_val_handle;  // 保存通知特征值的句柄

int is_connecting = 0;

static int gatt_svr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len,
               void *dst, uint16_t *len);

// 在文件开头添加回调函数类型定义
typedef void (*ble_data_callback_t)(const uint8_t *data, size_t length);

// 在文件开头添加函数声明
static void print_received_data(const uint8_t *data, size_t len, bool is_no_response);

// 在特征值定义时保存句柄
static uint16_t notify_handle;


// WiFi连接任务参数结构
typedef struct {
    char ssid[33];
    char password[65];
    uint8_t msg_id;
    uint16_t conn_handle;
    uint16_t notify_handle;
} wifi_connect_params_t;

// WiFi连接任务
static void wifi_connect_task(void *pvParameters)
{

    user_event_notify(USER_EVENT_WIFI_CONNECTING);
    is_connecting = 1;
    wifi_connect_params_t *params = (wifi_connect_params_t *)pvParameters;
    
    // 连接WiFi
    esp_err_t ret = wifi_connect_and_switch_to_sta(params->ssid, params->password);
    
    // 释放参数内存
    free(params);
    
    // 删除自己
    vTaskDelete(NULL);
}

static int
gatt_svr_chr_access_custom_service(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid16;
    int rc;

    uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    switch (uuid16) {
    case GATT_SVR_CHR_CUSTOM_READ_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *value = "Read Value";
            rc = os_mbuf_append(ctxt->om, value, strlen(value));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    case GATT_SVR_CHR_CUSTOM_WRITE_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t data[len];
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, len, NULL);
            if (rc == 0) {
                protocol_data_t result = protocol_parse_data(data, len);
                // result.cmd 打印
                if (result.success) {
                    ESP_LOGI(TAG, "Parsed command: %d\n %d", result.cmd, CMD_WIFI_CONFIG);
                    switch (result.cmd) {
                        case CMD_WIFI_CONFIG:
                            // 使用解析后的WiFi配置数据
                            wifi_config_t *wifi_config = &result.data.wifi_config;
                            wifi_connect_params_t *params = malloc(sizeof(wifi_connect_params_t));
                            if (params == NULL) {
                                ESP_LOGE(TAG, "Failed to allocate memory for WiFi connect params");
                                break;
                            }
                            
                            // 复制参数
                            strncpy(params->ssid, wifi_config->ssid, sizeof(params->ssid) - 1);
                            strncpy(params->password, wifi_config->password, sizeof(params->password) - 1);

                            // 复制UID
                            char uid[33] = {0};
                            strncpy(uid, wifi_config->uid, sizeof(uid) - 1);
                            uid[sizeof(uid) - 1] = '\0';

                            storage_save_uid(uid);
                            params->msg_id = result.msg_id;
                            
                            // 构建响应包
                            uint8_t response[21];  // sizeof(wifi_config_response_t)
                            size_t resp_len = pack_wifi_config_response(
                                0,
                                result.msg_id,
                                RESP_STATUS_OK,
                                response,
                                sizeof(response)
                            );
                            
                            ESP_LOGI(TAG, "WiFi config resp_len: %d", resp_len);

                            if (resp_len > 0) {
                                // 发送响应
                                ble_send_notify(response, resp_len);
                                ESP_LOGI(TAG,"WiFi config response sent");

                                if (is_connecting == 0) {
                                    // 创建WiFi连接任务
                                    // 清除重启标志 防止密码错误没提示
                                    clear_restart_flag();
                                    xTaskCreate(wifi_connect_task,
                                            "wifi_connect",
                                            4096,
                                            params,
                                            5,
                                            NULL);
                                }
                                
                            }
                            break;
                    }
                }
            }
            return 0;
        }
        break;

    case GATT_SVR_CHR_CUSTOM_INDICATE_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *value = "Indicate Value";
            rc = os_mbuf_append(ctxt->om, value, strlen(value));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    case GATT_SVR_CHR_CUSTOM_WRITE_NR_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            uint8_t data[len];
            rc = ble_hs_mbuf_to_flat(ctxt->om, data, len, NULL);
            if (rc == 0) {
                print_received_data(data, len, true);
            }
            return 0;
        }
        break;

    case GATT_SVR_CHR_CUSTOM_NOTIFY_UUID:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *value = "Notify Value";
            rc = os_mbuf_append(ctxt->om, value, strlen(value));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        break;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /*** Service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_SVR_SVC_CUSTOM_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) { 
            {
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = BLE_UUID16_DECLARE(GATT_SVR_CHR_CUSTOM_READ_UUID),
                .access_cb = gatt_svr_chr_access_custom_service,
                .flags = BLE_GATT_CHR_F_READ,
            },{
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = BLE_UUID16_DECLARE(GATT_SVR_CHR_CUSTOM_WRITE_UUID),
                .access_cb = gatt_svr_chr_access_custom_service,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },{
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = BLE_UUID16_DECLARE(GATT_SVR_CHR_CUSTOM_INDICATE_UUID),
                .access_cb = gatt_svr_chr_access_custom_service,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE,
            },{
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = BLE_UUID16_DECLARE(GATT_SVR_CHR_CUSTOM_WRITE_NR_UUID),
                .access_cb = gatt_svr_chr_access_custom_service,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },{
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = BLE_UUID16_DECLARE(GATT_SVR_CHR_CUSTOM_NOTIFY_UUID),
                .access_cb = gatt_svr_chr_access_custom_service,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &notify_chr_val_handle,  // 保存句柄
            },{
                0, /* No more characteristics */
            }
        },
    },

    {
        0, /* No more services. */
    },
};

static int
gatt_svr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len,
               void *dst, uint16_t *len)
{
    uint16_t om_len;
    int rc;

    om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}
void
gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int
gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    // ble_svc_ans_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }
    ble_set_notify_handle(notify_handle);

    return 0;
}

// 在文件中添加函数实现
static void print_received_data(const uint8_t *data, size_t len, bool is_no_response) {
    if (is_no_response) {
        ESP_LOGI(TAG, "Received no-response data (len=%d)\n", len);
    } else {
        ESP_LOGI(TAG, "Received data (len=%d)\n", len);
    }
    
}

uint16_t get_notify_chr_val_handle() {
    return notify_chr_val_handle;
}