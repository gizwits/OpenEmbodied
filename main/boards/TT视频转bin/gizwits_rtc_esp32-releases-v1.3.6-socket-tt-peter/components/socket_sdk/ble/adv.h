#ifndef BLE_ADV_H
#define BLE_ADV_H

#include <stdint.h>
#include "esp_err.h"

#include <stdbool.h>
#include "nimble/ble.h"
#include "modlog/modlog.h"
#include "esp_peripheral.h"
#include "gatt_svr.h"
#ifdef __cplusplus
extern "C" {
#endif

#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00
#define BLE_GAP_URI_PREFIX_HTTPS 0x17
#define VERSION_NUMBER      0x05    // 版本号为 5
#define DEVICE_TYPE_GATT   0x0B    // GATT 设备类型 (0b1011)
#define VERSION_TYPE       ((DEVICE_TYPE_GATT << 4) | VERSION_NUMBER)

// 功能掩码定义
#define BLE_VERSION_5_0    0x02    // BLE 5.0
#define BLE_VERSION_4_2    0x01    // BLE 4.2
#define SUPPORT_OTA        0x08    // 支持 OTA
#define SECURITY_AUTH      0x10    // 进行安全认证
#define NO_SECURITY_AUTH      0x00    // 不进行安全认证
#define ONE_DEVICE_SECRET  0x20    // 一机一密
#define NETWORK_CONFIG     0x40    // 配网标识

// 广播包中各字段的偏移量
#define FLAGS_INDEX        0
#define SERVICE_UUID1_INDEX  3
#define SERVICE_UUID2_INDEX  7
#define NAME_INDEX        11
#define CUSTOM_DATA_INDEX (NAME_INDEX + 2)  // 名称长度后的位置
#define VERSION_TYPE_INDEX (CUSTOM_DATA_INDEX + 4)  // Company ID 后的位置


struct ble_hs_cfg;
struct ble_gatt_register_ctxt;

typedef int ble_instance_cb_fn(uint16_t instance);


void ble_multi_advertise(ble_addr_t addr);

struct ble_instance_cb_register {
    ble_addr_t addr;
    ble_instance_cb_fn *cb;
};
void ble_set_network_status(bool configured);
esp_err_t ble_gen_adv_data(const char *device_name, uint32_t pk, const uint8_t *mac);
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg);
int gatt_svr_init(void);
void
start_connectable_ext(void);

#ifdef __cplusplus
}
#endif

#endif