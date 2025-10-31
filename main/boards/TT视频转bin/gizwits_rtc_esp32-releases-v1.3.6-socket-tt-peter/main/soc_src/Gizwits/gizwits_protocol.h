/**
************************************************************
* @file         gizwits_protocol.h
* @brief        Gizwits protocol related files (SDK API interface function definition)
* @author       Gizwits
* @date         2017-07-19
* @version      V03030000
* @copyright    Gizwits
*
* @note         机智云.只为智能硬件而生
*               Gizwits Smart Cloud  for Smart Products
*               链接|增值ֵ|开放|中立|安全|自有|自由|生态
*               www.gizwits.com
*
***********************************************************/

#ifndef _GIZWITS_PROTOCOL_H
#define _GIZWITS_PROTOCOL_H


#include <stdint.h>
#include "common.h"
#include "interface.h"
#include <stdbool.h>
#include "audio_processor.h"


/**
* @name Protocol version number
* @{
*/
#define PROTOCOL_VERSION              "00000004"
/**@} */

/**@name P0 protocol version number
* @{
*/
#define P0_VERSION                    "00000002"
/**@} */

/**@name Product Key (Product identification code)
* @{
*/
#define PRODUCT_KEY "8179cb7ac34649fe9eaa735892aed562"
/**@} */
/**@name Product Secret (Product key)
* @{
*/
#define PRODUCT_SECRET "57125a1eee4c484ebbd806406201dc5a"

#define MAC_LEN_8266                  6
#define BUFFER_LEN_MAX                900
#define REPORT_TIME_MAX               6000          ///< 6s

/**@name Timer correlation status
* @{
*/
#define MAX_SOC_TIMOUT                1             ///< 1ms
#define TIM_REP_TIMOUT                600000        ///< 600s Regularly report
/**@} */

/**@name Task related definitions
* @{
*/
//#define SIG_ISSUED_DATA               0x01          ///< Protocol processing tasks
//#define SIG_PASSTHROUGH               0x02          ///< Protocol transparent task
#define SIG_IMM_REPORT                0x03          ///< The agreement immediately reports the task
#define SIG_UPGRADE_DATA              0x04
#define SIG_BLE_0               0x05
#define SIG_BLE_1               0x06
#define SOFTAP                  0x07
#define AIRLINK                 0x08
#define RESET                   0x09
#define MSG_REQ                 0x10    // 请求消息推送
#define MSG_PLAYED              0x11    // 播放完了的上报
#define MSG_RECEIVE             0x12    // 收到消息
#define MSG_ABORT               0x13    // 消息中止
#define MSG_FAILED              0x14    // 消息失败
#define MSG_SAVE_VOLUME         0x20    // 保存音量
#define MSG_HALL_ON_PLAY        0x21    // 开盖播放
#define MODE_SINGLE 1
#define MODE_DUAL 2

/**@} */


/**@name Whether the device is in the control class, 0 means no, 1 means yes
* @{
*/
#define DEV_IS_GATEWAY   0//中控设备   
#define DEV_IS_GATEWAY2  0//网关设备      
/**@} */

/**@name Binding time
* @{
*/
#define NINABLETIME  0
/**@} */



#define MAX_PACKAGE_LEN    (sizeof(devStatus_t)+sizeof(dataPointFlags_t)+50)///< Data buffer maximum length
#define RB_MAX_LEN          (MAX_PACKAGE_LEN*2)     ///< Maximum length of ring buffer

#ifdef DATA_CONFIG_ENABLE
#define GIZ_DATA_LEN        (sizeof(AP_NAME)+sizeof(AP_PWD)+sizeof(CFG_MODE)+30)        ///< Maximum length of Environmental parameters
#endif

/**@name Data point related definition
* @{
*/
#define msg_req_LEN                           1
#define msg_flag_LEN                           1
#define state_LEN                           2
#define charge_status_LEN                           2
#define msg_type_LEN                           2
#define msg_status_LEN                           2

#define volume_RATIO                         1
#define volume_ADDITION                      0
#define volume_MIN                           0
#define volume_MAX                           100
#define volume_LEN                           1
#define volume_delta_RATIO                         1
#define volume_delta_ADDITION                      -100
#define volume_delta_MIN                           0
#define volume_delta_MAX                           200
#define volume_delta_LEN                           1
#define battery_percentage_RATIO                         1
#define battery_percentage_ADDITION                      0
#define battery_percentage_MIN                           0
#define battery_percentage_MAX                           100
#define battery_percentage_LEN                           1
#define msg_timestamp_RATIO                         1
#define msg_timestamp_ADDITION                      1735689600
#define msg_timestamp_MIN                           0
#define msg_timestamp_MAX                           2366755200
#define msg_timestamp_LEN                           4

#define msg_id_LEN                           19
#define msg_url_LEN                           256
/**@} */


/** Writable data points Boolean and enumerated variables occupy byte size */
#define COUNT_BIT 2


/** Writable data points Boolean and enumerated variables occupy byte size */
#define COUNT_W_BIT 1

/** Read-only data points Boolean and enumerated variables occupy byte size */
#define COUNT_R_BIT 1

#define DATAPOINT_FLAG_LEN sizeof(dataPointFlags_t)		 ///< All data points FLAG occupies the maximum number of bytes

typedef enum
{
    state_VALUE0_close = 0,//close
    state_VALUE1_standby = 1,//standby
    state_VALUE2_running = 2,//running
    state_VALUE_MAX,
} state_ENUM_T;
typedef enum
{
    charge_status_VALUE0_none = 0,//none
    charge_status_VALUE1_charging = 1,//charging
    charge_status_VALUE2_charge_done = 2,//charge_done
    charge_status_VALUE_MAX,
} charge_status_ENUM_T;
typedef enum
{
    msg_type_VALUE0_none = 0,//none
    msg_type_VALUE1_audio = 1,//audio
    msg_type_VALUE2_timing = 2,//timing
    msg_type_VALUE3_countdown = 3,//countdown
    msg_type_VALUE_MAX,
} msg_type_ENUM_T;
typedef enum
{
    msg_status_VALUE0_received = 0,//received
    msg_status_VALUE1_played = 1,//played
    msg_status_VALUE2_failed = 2,//failed
    msg_status_VALUE3_aborted = 3,//aborted
    msg_status_VALUE_MAX,
} msg_status_ENUM_T;

/** Event enumeration */
typedef enum
{
    WIFI_SOFTAP = 0x00,                               ///< WiFi SOFTAP configuration event
    WIFI_AIRLINK,                                     ///< WiFi module AIRLINK configuration event
    WIFI_STATION,                                     ///< WiFi module STATION configuration event
    WIFI_OPEN_BINDING,                                ///< The WiFi module opens the binding event
    WIFI_CLOSE_BINDING,                               ///< The WiFi module closes the binding event
    WIFI_CON_ROUTER,                                  ///< The WiFi module is connected to a routing event
    WIFI_DISCON_ROUTER,                               ///< The WiFi module has been disconnected from the routing event
    WIFI_CON_M2M,                                     ///< The WiFi module has a server M2M event
    WIFI_DISCON_M2M,                                  ///< The WiFi module has been disconnected from the server M2M event
    WIFI_OPEN_TESTMODE,                               ///< The WiFi module turns on the test mode event
    WIFI_CLOSE_TESTMODE,                              ///< The WiFi module turns off the test mode event
    WIFI_CON_APP,                                     ///< The WiFi module connects to the APP event
    WIFI_DISCON_APP,                                  ///< The WiFi module disconnects the APP event
    WIFI_RSSI,                                        ///< WiFi module RSSI event
    WIFI_NTP,                                         ///< Network time event
    MODULE_INFO,                                      ///< Module information event
    TRANSPARENT_DATA,                                 ///< Transparency events
    EVENT_msg_flag,
    EVENT_state,
    EVENT_msg_type,
    EVENT_volume,
    EVENT_volume_delta,
    EVENT_msg_timestamp,
    EVENT_msg_id,
    EVENT_msg_url,
    EVENT_TYPE_MAX                                    ///< Enumerate the number of members to calculate (user accidentally deleted)
} EVENT_TYPE_T;

/** P0 command command code*/
typedef enum
{
    ACTION_CONTROL_DEVICE       = 0x11,             ///< Write data points, that is, control devices or issue commands to devices.APP => Device
    ACTION_READ_DEV_STATUS      = 0x12,             ///< Read data points.APP => Device
    ACTION_READ_DEV_STATUS_ACK  = 0x13,             ///< Device replies to data point content.Device => APP
    ACTION_REPORT_DEV_STATUS    = 0x14,             ///< The device regularly (every 10 minutes) or proactively reports the current status when the status changes.Device => APP
    ACTION_W2D_TRANSPARENT_DATA = 0x05,             ///< App transparently transmits business instructions to device.APP => Device
    ACTION_D2W_TRANSPARENT_DATA = 0x06,             ///< Device transmits business instructions to App transparently.Device => APP
} actionType_t;

/** Protocol network time structure */
typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint32_t ntp;
}protocolTime_t;

/** WiFi module configuration parameters */
typedef enum
{
  WIFI_RESET_MODE = 0x00,                           ///< WIFI module reset
  WIFI_SOFTAP_MODE,                                 ///< WIFI module softAP mode
  WIFI_AIRLINK_MODE,                                ///< WIFI module AirLink mode
  WIFI_PRODUCTION_TEST,                             ///< The MCU requests the WiFi module to enter the production mode
  WIFI_NINABLE_MODE,                                ///< The MCU requests the module to enter binding mode
  WIFI_REBOOT_MODE,                                 ///< MCU request module reboot
}WIFI_MODE_TYPE_T;

/** WiFi module working status */
typedef union
{
    uint16_t value;
    struct
    {
        uint16_t            softap:1;               ///< Indicates the state of the SOFTAP mode in which the WiFi module is located, with the type bool
        uint16_t            station:1;              ///< Indicates the STATION mode status of the WiFi module, type bool
        uint16_t            onboarding:1;           ///< Indicates the configuration status of the WiFi module, type bool
        uint16_t            binding:1;              ///< Indicates the binding status of the WiFi module, type bool
        uint16_t            con_route:1;            ///< Indicates the connection status of the WiFi module to the router, type bool
        uint16_t            con_m2m:1;              ///< Indicates the state of the WiFi module and the cloud m2m, type bool
        uint16_t            reserve1:2;             ///< Data bits are filled
        uint16_t            rssi:3;                 ///< Indicates the signal strength of the route, the type is a value
        uint16_t            app:1;                  ///< Indicates the connection status of the WiFi module to the APP side, type bool
        uint16_t            test:1;                 ///< Indicates the state of the field measurement mode in which the WiFi module is located, with the type bool
        uint16_t            reserve2:3;             ///< Data bits are filled
    }types;
} wifi_status_t;

#pragma pack(1)

/** User Area Device State Structure */
typedef struct {
    bool valuemsg_req;
    bool valuemsg_flag;
    uint32_t valuestate;
    uint32_t valuecharge_status;
    uint32_t valuemsg_type;
    uint32_t valuemsg_status;
    uint32_t valuevolume;
    int32_t valuevolume_delta;
    uint32_t valuebattery_percentage;
    uint32_t valuemsg_timestamp;
    uint8_t valuemsg_id[19];
    uint8_t valuemsg_url[256];
} dataPoint_t;

/** User Area Device State Structure */
typedef struct {
    uint8_t flagmsg_req:1;
    uint8_t flagmsg_flag:1;
    uint8_t flagstate:1;
    uint8_t flagcharge_status:1;
    uint8_t flagmsg_type:1;
    uint8_t flagmsg_status:1;
    uint8_t flagvolume:1;
    uint8_t flagvolume_delta:1;
    uint8_t flagbattery_percentage:1;
    uint8_t flagmsg_timestamp:1;
    uint8_t flagmsg_id:1;
    uint8_t flagmsg_url:1;
} dataPointFlags_t;

/** Current status of WiFi module */
typedef struct {
    uint8_t bitFeildBuf[COUNT_BIT];
    uint8_t valuevolume;
    uint8_t valuevolume_delta;
    uint8_t valuebattery_percentage;
    uint32_t valuemsg_timestamp;
    uint8_t valuemsg_id[19];
    uint8_t valuemsg_url[256];
} devStatus_t;

/** Wifi signal strength numerical structure */
typedef struct {
    uint8_t rssi;                                   ///< WIFI signal strength
}moduleStatusInfo_t;



typedef struct {                           
    dataPointFlags_t  devDatapointFlag;             ///< Data points
    devStatus_t devStatus;                          ///< Device full data point status data
}gizwitsElongateP0Form_t;  

typedef struct {                           
    uint8_t action;
    uint8_t data[sizeof(gizwitsElongateP0Form_t)];
}gizwitsP0Max_t;                                  
                                                    
/** Event queue structure */                               
typedef struct {                           
    uint8_t num;                                    ///< Number of queue members
    uint8_t event[EVENT_TYPE_MAX];                  ///< Queue member event content
}eventInfo_t;



/** Protocol processing related structure */
typedef struct
{
    uint8_t mac[MAC_LEN_8266];
    uint8_t connectM2MFlag;
    uint32_t timerMsCount;                          ///< Time Count (Ms)
    uint8_t transparentBuff[BUFFER_LEN_MAX];        ///< Transparent data storage area
    uint32_t transparentLen;                        ///< Transparent data length

    dataPointFlags_t  waitReportDatapointFlag;      ///< Store the data points FLAG to be reported
    uint8_t reportData[sizeof(gizwitsElongateP0Form_t)];    ///< Protocol to report actual data
    uint32_t reportDataLen;                         ///< The protocol reports the actual data length

    
    dataPoint_t gizCurrentDataPoint;                ///< Current device status data
    dataPoint_t gizLastDataPoint;                   ///< The last reported device status data

    eventInfo_t issuedProcessEvent;                 ///< Control events
}gizwitsProtocol_t;

#pragma pack()

/**@name User API interface
* @{
*/

void setConnectM2MStatus(uint8_t m2mFlag);
uint8_t getConnectM2MStatus(void);
void gizwitsSetMode(uint8_t mode);
uint32_t gizwitsGetTimeStamp(void);
protocolTime_t gizwitsGetNTPTime(void);
void gizwitsInit(void);
int8_t gizwitsHandle(dataPoint_t *dataPoint);
int32_t gizwitsPassthroughData(uint8_t * data, uint32_t len);
int32_t gizwitsUploadData(uint8_t *did,uint8_t *data, uint32_t dataLen);
void bleCloseHandle(void);
char getRunMode(void);
void setRunMode(char mode);
void set_need_wait_send_played(uint8_t enable);
uint8_t get_need_wait_send_played(void);

/**@} */

#endif