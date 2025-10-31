/*
    This example code is in the Public Domain (or CC0 licensed, at your option.)

    Unless required by applicable law or agreed to in writing, this
    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "gagent_soc.h"
#include "hal_key.h"

#include <string.h>
#include <stdlib.h>
#include "common.h"

void keyInit(void);

#if 0
void user_Init()
{
    struct devAttrs attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.mBindEnableTime = 0;
    attrs.mDevAttr[0] = 0x00;
    attrs.mDevAttr[1] = 0x01;
    attrs.mDevAttr[2] = 0x00;
    attrs.mDevAttr[3] = 0x00;
    attrs.mDevAttr[4] = 0x00;
    attrs.mDevAttr[5] = 0x00;
    attrs.mDevAttr[6] = 0x20;
    attrs.mDevAttr[7] = 0x00;
    memcpy(attrs.mstrDevHV, "00000001", 8);
    memcpy(attrs.mstrDevSV, "00000001", 8);
    memcpy(attrs.mstrP0Ver, "00000001", 8);

    memcpy(attrs.mstrProductKey, "2443f9bc28ef45ffb31d6c5c3b0118e9", 32);
    memcpy(attrs.mstrPKSecret, "5a0e9f57fbfc401ab9495ec072c3c53c", 32);

    memcpy(attrs.mstrProtocolVer, "00000001", 8);
    memcpy(attrs.mstrSdkVerLow, "02", 2);

    /********************************* GAgent deafult val *********************************/
    attrs.szWechatDeviceType = "gh_35dd1e10ab57";
    attrs.szGAgentSever = "api.gizwits.com";
    attrs.gagentSeverPort = "80";
    //attrs.szGAgentSoftApName            = "XPG-GAgent-";
    attrs.m2mKeepAliveS = "150";
    attrs.localHeartbeatIntervalS = "40";
    attrs.localTransferIntervalMS = "450";
    attrs.networkCardName = "ens33";
    attrs.configMode = "0";
    attrs.runMode = "1";
    /***************************************************************************************/
    gagentInit(attrs);
}
#endif
static void gagentMainPthread(void *arg);
static xTaskHandle gagentMainTask;
extern void gatNetEventCbV4(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP32C3 chip with %d CPU cores, WiFi%s%s, ",
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    tcpip_adapter_init();

    printf("IDF_VER:%s\r\n", IDF_VER);

    printf("A V4 version callback will be used\r\n");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &gatNetEventCbV4, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &gatNetEventCbV4, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &gatNetEventCbV4, NULL));

    BaseType_t iRet;
    iRet = xTaskCreate(&gagentMainPthread, "gagentMainPthread", 16 * 1024, NULL, 5, &gagentMainTask);
    printf("%s gagentMainPthread iRet:%d\r\n", __FUNCTION__, iRet);
}

static void gagentMainPthread(void *arg)
{
    // setRunMode(MODE_SINGLE); //配网后关闭蓝牙
    setRunMode(MODE_DUAL);   //配网后打开蓝牙 
    keyInit();
    gizwitsInit();
    vTaskDelete(gagentMainTask);
}

#if 0
int32_t gizIssuedProcess(uint8 *didPtr, uint8 *inData, uint32 inLen, uint8 *outData, int32 *outLen)
{
    return 0;
}

void gizWiFiStatus(unsigned short int status)
{
    unsigned short localStatus = ntohs(status);
    if ((localStatus & (1 << 5)) >> 5)
    {
        printf("connected m2m\r\n");
        // gagentSyncScenesListFile( "api.simon-cloud.com.cn",80,0,0,gagentSyncScenesListFileCb );
        // subDevsRegisterTest();
    }
}
#endif
#define KEY_TEST 1
#ifdef KEY_TEST

#define GPIO_KEY_NUM 2 // Total number of key members
#define KEY_0_IO_NUM 4 // GPIO number
// #define KEY_1_IO_NUM 18 // GPIO number
#define KEY_1_IO_NUM 5 // GPIO number

key_typedef_t *singleKey[GPIO_KEY_NUM]; ///< Defines a single key member array pointer
keys_typedef_t keys;                    ///< Defines the overall key module structure pointer

/**
 * Key1 key short press processing
 * @param none
 * @return none
 */
void key1ShortPress(void)
{

    if (gatGetBleConfig() != 0)
    {
        GIZWITS_LOG("#### key1 short press, close the ble. \n");
        system_os_post(USER_TASK_PRIO_2, SIG_BLE_0, 0);
    }
    else
    {
        GIZWITS_LOG("#### key1 short press, open the ble. \n");
        system_os_post(USER_TASK_PRIO_2, SIG_BLE_1, 0);
    }
}
/**
 * Key1 key presses a long press
 * @param none
 * @return none
 */
void key1LongPress(void)
{
    GIZWITS_LOG("#### key1 long press, reset mode.\n");
    system_os_post(USER_TASK_PRIO_2, RESET, 0); 
}

/**
 * Key2 key to short press processing
 * @param none
 * @return none
 */
void key2ShortPress(void)
{

    GIZWITS_LOG("#### KEY2 short press ,soft ap mode.\n");
    system_os_post(USER_TASK_PRIO_2, SOFTAP, 0); 
}

/**
 * Key2 button long press
 * @param none
 * @return none
 */
void key2LongPress(void)
{
    GIZWITS_LOG("#### key2 long press, airlink mode.\n");
    system_os_post(USER_TASK_PRIO_2, AIRLINK, 0);
}

/**
 * Key to initialize
 * @param none
 * @return none
 */
void keyInit(void)
{
    singleKey[0] = keyInitOne(KEY_0_IO_NUM, 0, 0, key1LongPress, key1ShortPress);
    singleKey[1] = keyInitOne(KEY_1_IO_NUM, 0, 0, key2LongPress, key2ShortPress);
    keys.singleKey = singleKey;
    keyParaInit(&keys);
}

#endif
