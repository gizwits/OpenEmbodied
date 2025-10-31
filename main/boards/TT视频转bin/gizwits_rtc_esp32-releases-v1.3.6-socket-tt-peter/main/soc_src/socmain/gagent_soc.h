#ifndef GAGENT_SOC_H
#define GAGENT_SOC_H
#include "gattypes.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
// #include "tcpip_adapter.h"
// #include "esp_smartconfig.h"
// #include "hal_key.h"

#include "gizwits_product.h"
#include "interface.h"

#define CONFIG_SOFTAP           0x01
#define CONFIG_AIRLINK 			0x02
#define CONFIG_TEST_MODE 		0x03

typedef void (*gagentUploadDataCb)( int32_t result,void *arg,uint8_t* pszDID);

typedef enum _GAgentKeyVal_t
{
    GIZ_DID,                    // did
    GIZ_PASSCODE,               // passcode
    GIZ_TZS,                    // 当前时区秒
    GIZ_GSER,                   // 当前服务器域名
}GAgentKeyVal_t;

typedef struct _regRest_st
{
    int32_t  result;    // 云端返回码，非0为失败。
    uint8_t  szDID[22+1];    // 成功为有效DID,否则为NULL
    uint8_t  szAuthKey[32+1];    // 成功为有效DID,否则为NULL
    uint8_t  szMac[32+1];
    uint8_t  szPasscode[32+1];
    uint8_t  szMeshId[32+1];
    uint8_t  dsid[8+1];
    uint8_t* msg;    // 登陆成功或者失败的相关信息，失败时此信息有助于定位问题
}regRest_st;

typedef struct _verInfo_st
{
    uint8_t  szDID[DID_LEN+1];
    uint8_t  type;
    uint8_t  hardVer[MCU_HARDVER_LEN+1];
    uint8_t  softVer[MCU_SOFTVER_LEN+1];
}verInfo_st;
/* typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t ntp;
}_tm;
 */
struct devAttrs
{
    unsigned short mBindEnableTime;
    unsigned char mstrProtocolVer[8];
    unsigned char mstrP0Ver[8];
    unsigned char mstrDevHV[8];
    unsigned char mstrDevSV[8];
    unsigned char mstrProductKey[32];
    unsigned char mstrPKSecret[32];
    unsigned char mDevAttr[8];
    unsigned char mstrSdkVerLow[2];
    //gagent 微信公众号ID,默认为机智云微信宠物屋ID
    char *szWechatDeviceType;
    //gagent 默认连接服务器域名
    char *szGAgentSever;
    //gagent 默认连接服务器端口，默认为80
    char *gagentSeverPort;
    //gagent softap Name ,默认值:XPG-GAgent-xxxx(后面4位为MAC后4位)
    char *szGAgentSoftApName;
    char *szGAgentSoftApName0;
    //gagent softap 密码 ,默认值:123456789，若内容为空则热点不加密;
    char *szGAgentSoftApPwd;
    //m2m keepalive 默认值 120s
    char *m2mKeepAliveS;
    //m2m 心跳间隔 默认值为 50s
    char *m2mHeartbeatIntervalS;
    //gagent 时区秒，默认为东八区:8*(60*60)
    char *timeZoneS;
    //串口心跳间隔S，默认值 55秒
    char *localHeartbeatIntervalS;
    //串口数据传送ACK时间，默认值600ms+数据长度耗时
    char *localTransferIntervalMS;
    //网卡名称
    char *networkCardName;
    // 默认配置模式
    char *configMode;
    //在没有配置信息的情况下扫描热点的最多次数
    char *tScanNum;
    //在没有配置信息的情况下扫描到指定热点是否连接，1:连接 0:不连接
    char *tCon;
    /* 默认APN,GPRS类模块专用 */
    char *apn;
    /* 默认APN username,GPRS类模块专用 */
    char *apnName;
    /* 默认APN password,GPRS类模块专用 */
    char *apnPwd;
    // 默认配置运行模式
    char *runMode;
};

/**************************************************************************************
 * gagentDevsRegisterCb
 * 子设备注册结果回调
 **************************************************************************************/
typedef void (*gagentDevsRegisterCb)(regRest_st result[],int32_t resultNum, void *arg);
/**************************************************************************************
 * gagentInit
 * 传入参数初始化GAgent，在例程user_init中调用
 **************************************************************************************/
void gagentInit(struct devAttrs attrs);
/**************************************************************************************
Function    :   gagentUploadData
说明        :   上传数据到客户端，包括大循环和小循环
szDID       :   要上传的数据对应的设备DID,目前的版本该值填NULL
src         :   要上传数据的内容指针,格式与flag参数的bit2~bit1有关
len         :   上传数据的大小
flag        :   bit7~bit6: 1上报到小循环; 2上报到大循环; 3大小循环都上报
                bit2~bit1: 0 0x93命令上报P0数据
                bit0     : 0 不关心数据上报结果，1 关心数据上报结果
arg         :   上传数据回调函数的参数，不能为NULL
fun         :   上传数据的结果回调函数
return      :   RET_FAILED/RET_SUCCESS
**************************************************************************************/
int32 gagentUploadData(uint8 *szDID, uint8 *src, uint32 len,uint8 flag, void *arg,gagentUploadDataCb fun );
/**************************************************************************************
 * gagentDevsRegister
 * 注册子设备
 * subDev: 子设备列表
 **************************************************************************************/
int32_t gagentDevsRegister( regRest_st* subDev,int32_t devNum, uint8_t* szPK,uint8_t* szPKS,uint8_t *pszParentDID, uint8_t *pGwDid,uint8_t *devExtra,void *arg,int8_t is_reset,gagentDevsRegisterCb fun );
/**************************************************************************************
 * GAgentInfoGet
 * 获取GAgent信息（did等）
 **************************************************************************************/
uint8_t *GAgentInfoGet( GAgentKeyVal_t key );
/**************************************************************************************
 * gagentGetNTP
 * 获取当前NTP时间
 **************************************************************************************/
void gagentGetNTP(_tm *time);
/**************************************************************************************
 * gagentConfig
 * 进入配置模式
 * configType:
 *  CONFIG_SOFTAP - 进入SoftAP和蓝牙配网模式
 *  CONFIG_AIRLINK - 进入AirLink配置模式
 *  CONFIG_TEST_MODE - 进入WIFI测试模式
 **************************************************************************************/
void gagentConfig(unsigned char configType);
/**************************************************************************************
 * gagentConfig
 * 恢复出厂配置
 **************************************************************************************/
void gagentReset(void);
/**************************************************************************************
 * GAgentEnableBind
 * 开启绑定模式，并设置超时
 **************************************************************************************/
void GAgentEnableBind(void);
/**************************************************************************************
 * gatBleConfig
 * 配置蓝牙使能 0:不使能 1:使能
 **************************************************************************************/
void gatBleConfig(uint8_t status);
/**************************************************************************************
 * gatGetBleStatus
 * 获取蓝牙使能状态 0:未使能 1:已使能
 **************************************************************************************/
uint8_t gatGetBleConfig(void);
/**************************************************************************************
 * gatGetBleStatus
 * 获取蓝牙连接状态 0:未连接 1:已连接
 **************************************************************************************/
uint8_t gatGetBleConnect(void);
/**************************************************************************************
 * gatWatchDogFeed
 * 用于看门狗喂狗
 ***************************************************************************************/
int gatWatchDogFeed();
/*****************************************************
 * @brief BLE广播包刷新函数，用于soc工程更新可配网状态
 * @return   no ret
 *****************************************************/
int32_t iofFlushGapScanRspData();

#endif