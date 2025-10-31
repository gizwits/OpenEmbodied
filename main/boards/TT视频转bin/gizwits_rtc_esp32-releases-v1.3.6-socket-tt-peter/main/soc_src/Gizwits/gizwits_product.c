/**
************************************************************
* @file         gizwits_product.c
* @brief        Control protocol processing, and platform-related hardware initialization
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
#include <stdio.h>
#include <string.h>
#include "gizwits_product.h"
// #include "hal_key.h"
#include "interface.h"
#include "audio_processor.h"
#include "esp_timer.h"
#include "mqtt.h"
#include "mp3_url_play.h"
#include "uart_ctrl_lcd.h"
#include "battery.h"
#include "tt_ledc.h"
#include "board/charge.h"
#include "coze_socket.h"
#include "mqtt.h"
#include "hall_switch.h"
#include "board/board_init.h"
#include "battery.h"
/* JW 产品逻辑*/
static uint8_t last_volume = 0;
static int16_t i16_volume = 0;
static uint8_t lastplay_index = 0xff;


void current_led_update(void)
{
    if(get_onboarding_on() == 0 || get_valuestate() == state_VALUE0_close)
    {
        tt_led_strip_set_state(TT_LED_STATE_OFF);
    }
    else
    {
        tt_led_strip_set_state(TT_LED_STATE_ORANGE);
    }
}


// 1. 消息推送定时处理

void showMsgRec(const MessageRecord* record) {
    printf("Message ID: ");
    for (int i = 0; i < 19; i++) {
        printf("%02X", record->msg_id[i]);
    }
    printf("\nMessage Type: %02X\n", record->msg_type);
    printf("Message Status: %02X\n", record->msg_status);
    printf("Message Content: %s\n", record->valuemsg_url);
}

#define MAX_RECORDS 1   // 只保存一条消息
static MessageRecord messageRecords[MAX_RECORDS];
static uint8_t recordIndex = 0xff;
typedef enum {
    MSG_STATE_IDLE,
    MSG_STATE_PLAYING,
    MSG_STATE_PLAYED
} MessageState;

MessageRecord* getMessageRecords() {
    if(recordIndex != 0xff) {
        return &messageRecords[recordIndex];
    }
    return NULL;
}

void reset_messageRecords_msg_id(void)
{
    if(lastplay_index != 0xff)
    {
        memset(messageRecords[lastplay_index].msg_id, 0, sizeof(messageRecords[lastplay_index].msg_id));
    }
}

static MessageState msgState = MSG_STATE_IDLE;



// 消息定时器
// 收到一条播一条，知道flag == 0 ，或者超时
static uint8_t need_wait_send_played = 0;   // 这个标志主要为了不让模组继续做msg_req
void set_need_wait_send_played(uint8_t enable)
{
    printf("%s %d\n", __func__, enable);
    need_wait_send_played = enable;
}
uint8_t get_need_wait_send_played()
{
    return need_wait_send_played;
}

static esp_timer_handle_t message_timer_play;

static void play_audio_task(void* arg) {
    const char* url = (const char*)arg;
    url_mp3_play(url);
    // 播完本地测试
    printf("%s end\n", __func__);
    vTaskDelete(NULL);
}

// 闹钟类型自删定时器
static esp_timer_handle_t timing_timeout_timer = NULL;
static void timing_timeout_timer_callback(void* arg) {
    system_os_post(USER_TASK_PRIO_2, MSG_FAILED, 0);
    // vTaskDelay(pdMS_TO_TICKS(1000));; // 延时一小会  切换到新的 callback 请不要删除这个代码

    // if(recordIndex != 0xff)
    // {
    //     showMsgRec(&messageRecords[0]);
    //     memset(&messageRecords[recordIndex], 0, sizeof(MessageRecord));
    //     showMsgRec(&messageRecords[0]);
    // }
    recordIndex = 0xff;
    lastplay_index = 0xff;
    timer_stop_delete(&message_timer_play);
    ESP_LOGI("Gizwits", "Message record at index %d has been reset", recordIndex);
    tt_led_strip_set_state(TT_LED_STATE_OFF);
}

const esp_timer_create_args_t timing_timeout_timer_args = {
    .callback = (esp_timer_cb_t)timing_timeout_timer_callback,
    .arg = NULL,
    .name = "timing_timeout_timer"
};

void play_mp3_result_report()
{
    // 播完了至改状态上报
    if (lastplay_index != 0xff) {
        if(!get_i2s_is_abort() && !get_audio_url_is_failed())
        {
            printf("Audio finished playing, updating status.\n");
            messageRecords[lastplay_index].msg_status = msg_status_VALUE1_played;
            currentDataPoint.valuemsg_status = msg_status_VALUE1_played;
            memcpy(currentDataPoint.valuemsg_id, messageRecords[lastplay_index].msg_id, sizeof(currentDataPoint.valuemsg_id));
            system_os_post(USER_TASK_PRIO_2, MSG_PLAYED, 0);
        }
        else if(get_i2s_is_abort())
        {
            set_i2s_is_abort(0);
            messageRecords[lastplay_index].msg_status = msg_status_VALUE3_aborted;
            printf("Audio[%.*s] is abort\n", sizeof(messageRecords[lastplay_index].msg_id), messageRecords[lastplay_index].msg_id);
            system_os_post(USER_TASK_PRIO_2, MSG_ABORT, 0);
        }
        else if(get_audio_url_is_failed())
        {
            set_audio_url_is_failed(0);
            messageRecords[lastplay_index].msg_status = msg_status_VALUE2_failed;
            printf("Audio[%.*s] is failed\n", sizeof(messageRecords[lastplay_index].msg_id), messageRecords[lastplay_index].msg_id);
            system_os_post(USER_TASK_PRIO_2, MSG_FAILED, 0);

        }
        lastplay_index = 0xff;
        recordIndex = 0xff;
    }

    showMsgRec(&messageRecords[0]);
}


static void message_play_timer_callback(void* arg) {
    printf("%s start\n", __func__);


    // 定时任务强制唤醒
    for (int i = 0; i < MAX_RECORDS; i++) {
        if (!audio_tone_url_is_playing()) 
        {
            showMsgRec(&messageRecords[i]);
        }
        if (messageRecords[i].msg_id[0] != 0 && messageRecords[i].msg_status == msg_status_VALUE0_received 
            &&messageRecords[i].msg_type == msg_type_VALUE2_timing)
        {
            // 没在播的时候唤醒播放
            if(!audio_tone_url_is_playing() && get_voice_sleep_flag() && get_hall_state() == HALL_STATE_ON && (get_valuestate() != state_VALUE0_close))
            {
                SET_WAKEUP_FLAG(true);
                set_voice_sleep_flag(false);
                break;
            }
        }
    }


    if(!audio_tone_url_is_playing()&&get_url_i2s_is_finished())
    {
        play_mp3_result_report();
    }

    // INSERT_YOUR_CODE
    if (/*get_voice_sleep_flag()||*/ audio_tone_url_is_playing()||get_is_playing_cache()||!get_i2s_is_finished()||
        !get_url_i2s_is_finished()||
        get_hall_state() == HALL_STATE_OFF || get_valuestate() == state_VALUE0_close || 
        get_valuestate() == state_VALUE1_standby) {
        printf("MagicI is %s, restarting timer. "
        "voice_sleep: %d, wakeup_flag: %d,"
        "is_playing_cache: %d,"
        "i2s_is_finished: %d,"
        "url_i2s_is_finished: %d,"
        "valuestate:%d\n",
         get_voice_sleep_flag()?"sleeping":get_valuestate() != state_VALUE2_running?"standby/close":"playing Audio",
         get_voice_sleep_flag(),
         get_wakeup_flag(),
         get_is_playing_cache(),
         get_i2s_is_finished(),
         get_url_i2s_is_finished(),
         get_valuestate());
        ESP_ERROR_CHECK(esp_timer_start_once(message_timer_play, 3000000)); // 3秒
        return;
    }
    // else {
    //     play_mp3_result_report();
    // }

    
    for (int i = 0; i < MAX_RECORDS; i++) {
        showMsgRec(&messageRecords[i]);
        if (messageRecords[i].msg_id[0] != 0 && messageRecords[i].msg_status == msg_status_VALUE0_received) {
            printf("Playing message with ID: ");
            for (int j = 0; j < sizeof(messageRecords[i].msg_id); j++) {
                printf("%02X", messageRecords[i].msg_id[j]);
            }
            printf("\n");
            // need_wait_send_played = 1;
            if(messageRecords[i].msg_type == msg_type_VALUE2_timing) {
                timer_stop_delete(&timing_timeout_timer);
            }

            xTaskCreate(play_audio_task, "play_audio_task", 4096, (void*)messageRecords[i].valuemsg_url, 10, NULL);
            lastplay_index = i;
            // 3秒后再检查
            printf("Message is playing, setting timer for 3 seconds.\n");
            esp_timer_start_once(message_timer_play, 3000000); // 3秒

            break;
        }
    }

}

void create_message_play_timer() {
    const esp_timer_create_args_t timer_args = {
        .callback = &message_play_timer_callback,
        .arg = NULL,
        .name = "message_timer_play"
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &message_timer_play));
    ESP_ERROR_CHECK(esp_timer_start_once(message_timer_play, 3000000)); // 3秒
}


// 2. 上电/唤醒 要请求语音消息，直到没有语音消息才连接websocket智能体
#if 0
static esp_timer_handle_t message_timer_req;

static void message_timer_req_callback(void* arg) {
    printf("%s start\n", __func__);
    // INSERT_YOUR_CODE
    if (audio_tone_url_is_playing()) {
        ESP_ERROR_CHECK(esp_timer_start_once(message_timer_play, 3000000)); // 3秒
        return;
    }

    if (currentDataPoint.valuemsg_req == 1) {
        system_os_post(USER_TASK_PRIO_2, MSG_REQ, 0);
    }

    printf("%s end\n", __func__);
}

void create_message_req_timer() {
    const esp_timer_create_args_t timer_args = {
        .callback = &message_timer_req_callback,
        .arg = NULL,
        .name = "message_timer_req"
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, NULL));
    ESP_ERROR_CHECK(esp_timer_start_once(message_timer_req, 3000000)); // 3秒
}

#endif

void set_msg_req(bool enable) {
    // 初始化要请求消息，当收到无消息才置零
    currentDataPoint.valuemsg_req = enable;
}

bool get_msg_req() {
    return currentDataPoint.valuemsg_req;
}

static esp_timer_handle_t req_timeout_timer;

static void req_timeout_timer_callback(void* arg) {
    printf("%s start\n", __func__);
    set_msg_req(0);
    printf("%s end\n", __func__);
}

void init_req_timeout_timer() {
    const esp_timer_create_args_t timer_args = {
        .callback = &req_timeout_timer_callback,
        .arg = NULL,
        .name = "req_timeout_timer"
    };

    if (req_timeout_timer == NULL) {
        esp_timer_create(&timer_args, &req_timeout_timer);
        esp_timer_start_once(req_timeout_timer, 5000000); // 5秒
    }
}

void delete_req_timeout_timer() {
    if (req_timeout_timer != NULL) {
        esp_timer_stop(req_timeout_timer);
        esp_timer_delete(req_timeout_timer);
        req_timeout_timer = NULL;
    }

    // 请求URL超时后, 如果没连上WS就重连
    if(get_socket_client() == NULL)
    {
        printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
        mqtt_sem_give();
    }
}



/* JW 产品逻辑*/



/** User area The current device state structure */
dataPoint_t currentDataPoint;
static bool refreshBle = true;
extern bool m2mFlag;
/**@name Gizwits User Interface
* @{
*/

/**
* @brief Event handling interface

* Description:

* 1. Users can customize the changes in WiFi module status

* 2. Users can add data points in the function of event processing logic, such as calling the relevant hardware peripherals operating interface

* @param [in] info: event queue
* @param [in] data: protocol data
* @param [in] len: protocol data length
* @return NULL
* @ref gizwits_protocol.h
*/


int8_t ICACHE_FLASH_ATTR gizwitsEventProcess(eventInfo_t *info, uint8_t *data, uint32_t len)
{
    uint8_t i = 0;
    dataPoint_t * dataPointPtr = (dataPoint_t *)data;
    moduleStatusInfo_t * wifiData = (moduleStatusInfo_t *)data;
    bool shouldCopyData = false;

    if((NULL == info) || (NULL == data))
    {
        GIZWITS_LOG("!!! gizwitsEventProcess Error \n");
        return -1;
    }

    for(i = 0; i < info->num; i++)
    {
        switch(info->event[i])
        {
        case EVENT_msg_flag :
            GIZWITS_LOG("Evt: EVENT_msg_flag %d,  cur %d \n",
                dataPointPtr->valuemsg_flag, currentDataPoint.valuemsg_flag);
            currentDataPoint.valuemsg_flag = dataPointPtr->valuemsg_flag;
            currentDataPoint.valuemsg_req = currentDataPoint.valuemsg_flag;
            // if(audio_tone_url_is_playing()) {
            //     GIZWITS_LOG("!!! audio_tone_url_is_playing\n");
            //     currentDataPoint.valuemsg_req = 1;
            //     continue;
            // }
            // 播放中不处理msg_flag
            if(currentDataPoint.valuemsg_flag && audio_tone_url_is_playing() == 0)
            {
                showMsgRec(&messageRecords[0]);
                if(recordIndex == 0xff || messageRecords[lastplay_index].msg_id[0] == 0)  // 没播 或 播完
                {
                    shouldCopyData = 1;
                }
                GIZWITS_LOG("%s %d shouldCopyData %d recordIndex %d \n", __func__, __LINE__, shouldCopyData, recordIndex);
            }
            else
            {
                GIZWITS_LOG("%s %d is playing url\n", __func__, __LINE__);
                break;
            }

            // // URL播放中不处理推送
            // if(shouldCopyData && !audio_tone_url_is_playing())
            // {
            //     shouldCopyData = 0;
            // }

            // 需要判断 info->num 是否大于4？（id type url flag）
            // 单次不收齐，则不存储
            if (shouldCopyData  && info->num >= 4) {
                // 存储msg_id, msg_type, msg_status, msg_content
                #if 0
                for (int j = 0; j < MAX_RECORDS; j++) {
                    // 如果msg_id[0]为0，则记录当前索引
                    if (messageRecords[j].msg_id[0] == 0) {
                        recordIndex = j;
                        break;
                    }
                    // 如果msg_id[0]不为0，但是播放了，则清空成员
                    if (messageRecords[j].msg_id[0] != 0 && messageRecords[j].msg_status != msg_status_VALUE0_received) {
                        memset(&messageRecords[j], 0, sizeof(MessageRecord)); // 清空成员
                        recordIndex = j;
                        break;
                    }
                }
                if(recordIndex == 0xff) {
                    GIZWITS_LOG("messageRecords full !\n");
                    shouldCopyData = false;
                }
                #else
                recordIndex = 0;
                #endif
                messageRecords[recordIndex].msg_status = msg_status_VALUE0_received;
                need_wait_send_played = 1;

            }


            // 如果消息标志为0, 且不在播放中，则重连
            if(currentDataPoint.valuemsg_flag == 0 && audio_tone_url_is_playing() == 0)
            {
                if(get_socket_client() == NULL)
                {
                    GIZWITS_LOG(" client is null \n");
                    printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
                    mqtt_sem_give();
                }
                else
                {
                    GIZWITS_LOG(" client is not null \n");
                }
            }
            // 收到消息就可以删除超时定时器了
            delete_req_timeout_timer();

            break;

        case EVENT_state:   // todo peter mark
            currentDataPoint.valuestate = dataPointPtr->valuestate;
            GIZWITS_LOG("Evt: EVENT_state %d\n", currentDataPoint.valuestate);
            switch(currentDataPoint.valuestate)
            {
            case state_VALUE0_close:
            #if 0
                //user handle
                led_effect_stop();
                tt_led_strip_set_state(TT_LED_STATE_OFF);
                lcd_state_event_send(EVENT_OFF);
                // set_manual_break_flag(true); // 这是打断当前AI讲话
                
                // 防止被AI回复再次唤醒
                SET_WAKEUP_FLAG(false);
                send_conversation_chat_cancel("gizwitsEventProcess");

                if (get_battery_state() == BATTERY_NOT_CHARGING){
                    audio_tone_play(0, 0, "spiffs://spiffs/bo.mp3");
                    // vTaskDelay(pdMS_TO_TICKS(2000));
                    gpio_set_power_status(0);
                }
                else
                {
                }
            #else
                void close_device(bool is_delayed);
                close_device(false);
            #endif
                break;
            case state_VALUE1_standby:
                SET_WAKEUP_FLAG(false);
                run_sleep();
                //user handle
                break;
            case state_VALUE2_running:
                if(get_hall_state() == HALL_STATE_ON)
                {
                    set_msg_req(1);
                    lcd_state_event_send(EVENT_WAKEUP);
                    // tt_led_strip_set_state(TT_LED_STATE_WHITE);
                    SET_WAKEUP_FLAG(true);
                    audio_tone_play(0, 0, "spiffs://spiffs/bo.mp3");
                }
                else
                {
                    GIZWITS_LOG(" hall is close..\n");
                }
                //user handle
                break;
            default:
                break;
            }
            break;
        case EVENT_msg_type:
            GIZWITS_LOG("Evt: EVENT_msg_type %d\n", currentDataPoint.valuemsg_type);
            if (shouldCopyData) {
                messageRecords[recordIndex].msg_type = dataPointPtr->valuemsg_type;
                currentDataPoint.valuemsg_type = dataPointPtr->valuemsg_type;
            }
            else
            {
                GIZWITS_LOG("%s %d is playing url\n", __func__, __LINE__);
                break;
            }
            switch(currentDataPoint.valuemsg_type)
            {
            case msg_type_VALUE0_none:
                //user handle
                break;
            case msg_type_VALUE1_audio:
                //user handle
                break;
            case msg_type_VALUE2_timing:

                //user handle
                break;
            case msg_type_VALUE3_countdown:
                //user handle
                break;
            default:
                break;
            }
            break;

        case EVENT_volume:

            // 限制音量范围
            if(dataPointPtr->valuevolume > LOGIC_MAX_VOLUME)
            {
                dataPointPtr->valuevolume = LOGIC_MAX_VOLUME;
            }
            else if(dataPointPtr->valuevolume < LOGIC_MIN_VOLUME)
            {
                dataPointPtr->valuevolume = LOGIC_MIN_VOLUME;
            }

            // 计算音量变化
            set_valuevolume_delta(dataPointPtr->valuevolume - currentDataPoint.valuevolume);

            // 设置音量
            user_set_volume_no_nvs(dataPointPtr->valuevolume);
            
            // 更新当前音量
            currentDataPoint.valuevolume = dataPointPtr->valuevolume;
            GIZWITS_LOG("Evt:EVENT_volume %d\n",currentDataPoint.valuevolume);

            // 提示音量最大/最小
            user_volume_set_tone_play();
            break;
        case EVENT_volume_delta:
            GIZWITS_LOG("Evt:EVENT_volume_delta set[%d],now[%d]\n", \
                dataPointPtr->valuevolume_delta, currentDataPoint.valuevolume_delta);

            last_volume = currentDataPoint.valuevolume;
            GIZWITS_LOG("Previous volume: %d\n", last_volume);
            
            i16_volume = currentDataPoint.valuevolume;
            i16_volume += dataPointPtr->valuevolume_delta;
            GIZWITS_LOG("Updated volume before limit check: %d\n", i16_volume);

            // 限制音量范围
            if(i16_volume > LOGIC_MAX_VOLUME)
            {
                i16_volume = LOGIC_MAX_VOLUME;
                GIZWITS_LOG("Volume exceeded max, set to max: %d\n", LOGIC_MAX_VOLUME);
            }
            else if(i16_volume < LOGIC_MIN_VOLUME)
            {
                i16_volume = LOGIC_MIN_VOLUME;
                GIZWITS_LOG("Volume below min, set to min: %d\n", LOGIC_MIN_VOLUME);
            }
            GIZWITS_LOG("Volume after limit check: %d\n", i16_volume);
            currentDataPoint.valuevolume_delta = i16_volume - last_volume;
            currentDataPoint.valuevolume = i16_volume;

            GIZWITS_LOG("Volume delta after limit check: %d\n", currentDataPoint.valuevolume_delta);

            // 设置音量
            user_set_volume_no_nvs(currentDataPoint.valuevolume);   // 不能使用user_set_volume，得在外部写
            GIZWITS_LOG("Volume set to: %d\n", currentDataPoint.valuevolume);

            // 提示音量最大/最小
            user_volume_set_tone_play();


            //user handle
            break;

        case EVENT_msg_id:
            GIZWITS_LOG("Evt: EVENT_msg_id\n");
            gizMemcpy((uint8_t *)&currentDataPoint.valuemsg_id,(uint8_t *)&dataPointPtr->valuemsg_id,sizeof(currentDataPoint.valuemsg_id));

            if (shouldCopyData) {
                gizMemcpy((uint8_t *)&currentDataPoint.valuemsg_id,(uint8_t *)&dataPointPtr->valuemsg_id,sizeof(currentDataPoint.valuemsg_id));
                gizMemcpy(messageRecords[recordIndex].msg_id, dataPointPtr->valuemsg_id, sizeof(dataPointPtr->valuemsg_id));
            }
            else
            {
                GIZWITS_LOG("%s %d is playing url\n", __func__, __LINE__);
                break;
            }
            //user handle
            break;

        case EVENT_msg_url:
            GIZWITS_LOG("Evt: EVENT_msg_url\n");
            if (shouldCopyData) {
                gizMemcpy((uint8_t *)&currentDataPoint.valuemsg_url,(uint8_t *)&dataPointPtr->valuemsg_url,sizeof(currentDataPoint.valuemsg_url));
                gizMemcpy(messageRecords[recordIndex].valuemsg_url, dataPointPtr->valuemsg_url, sizeof(dataPointPtr->valuemsg_url));
            }
            else
            {
                GIZWITS_LOG("%s %d is playing url\n", __func__, __LINE__);
                break;
            }
            //user handle
            break;

        case WIFI_SOFTAP:
            refreshBle = true;
            break;
        case WIFI_AIRLINK:
            refreshBle = true;
            break;
        case WIFI_STATION:
            break;
        case WIFI_CON_ROUTER:
            GIZWITS_LOG("@@@@ connected router\n");
            break;
        case WIFI_DISCON_ROUTER:
            GIZWITS_LOG("@@@@ disconnected router\n");
            break;
        case WIFI_CON_M2M:
            GIZWITS_LOG("@@@@ connected m2m\n");
            setConnectM2MStatus(0x01);
            break;
        case WIFI_DISCON_M2M:
            GIZWITS_LOG("@@@@ disconnected m2m\n");
            setConnectM2MStatus(0x00);
            break;
        case WIFI_RSSI:
            GIZWITS_LOG("@@@@ RSSI %d\n", wifiData->rssi);
            break;
        case TRANSPARENT_DATA:
            GIZWITS_LOG("TRANSPARENT_DATA \n");
            //user handle , Fetch data from [data] , size is [len]
            break;
        case MODULE_INFO:
            GIZWITS_LOG("MODULE INFO ...\n");
            break;
            
        default:
            break;
        }
    }

    if(shouldCopyData && recordIndex != 0xff && messageRecords[recordIndex].msg_id[0] != 0 && messageRecords[recordIndex].valuemsg_url[0] != 0) {
        GIZWITS_LOG("---------new message record---------\n");
        messageRecords[recordIndex].msg_status = msg_status_VALUE0_received;
        showMsgRec(&messageRecords[recordIndex]);
        if(message_timer_play == NULL) {
            create_message_play_timer();
        }
        else {
            // 如果定时器已经存在，则重新启动定时器，等待本地提示语音播完
            esp_timer_start_once(message_timer_play, 3000000); // 3秒
        }
        currentDataPoint.valuemsg_flag = 0;

        // 收齐了msg_id和msg_url，则上报
        system_os_post(USER_TASK_PRIO_2, MSG_RECEIVE, 0);
        tt_led_strip_set_state(TT_LED_STATE_PURPLE);

        // 如果是定时任务，则启动超时定时器
        if( messageRecords[recordIndex].msg_type == msg_type_VALUE2_timing) {
            timer_start(&timing_timeout_timer, &timing_timeout_timer_args, 60000);  // 1 minute in milliseconds
            // if(get_voice_sleep_flag() && get_hall_state() == HALL_STATE_ON)
            // {
            //     SET_WAKEUP_FLAG(true);
            //     set_voice_sleep_flag(false);
            // }
        }
        else
        {
            timer_stop_delete(&timing_timeout_timer);
        }

    }
    system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    return 0; 
}


/**
* User data acquisition

* Here users need to achieve in addition to data points other than the collection of data collection, can be self-defined acquisition frequency and design data filtering algorithm

* @param none
* @return none
*/
const char* get_signal_level_str(int32_t rssi) {
    if (rssi >= -50) return "Excellent ( rssi >= -50)";
    else if (rssi >= -60) return "Good (-50: -60)";
    else if (rssi >= -70) return "Medium (-60: -70)";
    else if (rssi >= -80) return "Low (-70: -80)";
    else return "Bad (-80 > rssi)";
}
void printf_cur_flag()
{
    char mac_str[13];
    get_mac_str(mac_str, true);
    product_info_t *pInfo = get_product_info();
    wifi_ap_record_t ap_info;
    int32_t rssi;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi; // 获取信号强度
    }
    else
    {
        rssi = 0;
    }
    printf("-----------------------------------\n"
        "currentDataPoint.valuemsg_req: %d\n"
        "battery_get_voltage: %d\n"
        "battery_get_estimate: %d\n"
        "audio_tone_url_is_playing: %d\n"
        "get_need_wait_send_played: %d\n"
        "get_hall_state: %d\n"
        "get_socket_client: %d\n"
        "get_mqtt_is_connected: %d\n"
        "get_voice_sleep_flag: %d\n"
        "get_i2s_is_finished: %d\n"
        "get_url_i2s_is_finished: %d\n"
        "get_is_playing_cache: %d\n"
        "get_valuestate: %d\n"
        "get_wakeup_flag: %d\n"
        "get_manual_break_flag: %d\n"
        "WIFI-RSSI: %d (Signal Level: %s)\n"
        "-----------------------------------\n",
        currentDataPoint.valuemsg_req, battery_get_voltage(), battery_get_estimate(TYPE_AVERAGE), 
        audio_tone_url_is_playing(), get_need_wait_send_played(), get_hall_state(),
        get_socket_client(), get_mqtt_is_connected(), get_voice_sleep_flag(), get_i2s_is_finished(),
        get_url_i2s_is_finished(),
        get_is_playing_cache(), get_valuestate(), get_wakeup_flag(), get_manual_break_flag(), rssi,
        get_signal_level_str(rssi));
}
void ICACHE_FLASH_ATTR userHandle(void)
{
    /*
    currentDataPoint.valuecharge_status = ;//Add Sensor Data Collection
    currentDataPoint.valuemsg_status = ;//Add Sensor Data Collection
    currentDataPoint.valuebattery_percentage = ;//Add Sensor Data Collection

    */
    // 关机不做事
    if(currentDataPoint.valuestate == state_VALUE0_close)
    {
        current_led_update();
        return;
    }

    static uint8_t times = 1,i = 0;
    times++;

    if (times % 10 == 0)   //定时30s改变一次数据点并上报
    {
        // todo test
        
        // currentDataPoint.valuemsg_req = !currentDataPoint.valuemsg_req;
        // currentDataPoint.valuemsg_flag = !currentDataPoint.valuemsg_flag;
        // currentDataPoint.valuestate++;
        // currentDataPoint.valuestate %= state_VALUE_MAX;
        // currentDataPoint.valuecharge_status++;
        // currentDataPoint.valuecharge_status %= charge_status_VALUE_MAX;
        // currentDataPoint.valuemsg_type++;
        // currentDataPoint.valuemsg_type %= msg_type_VALUE_MAX;
        // currentDataPoint.valuemsg_status++;
        // currentDataPoint.valuemsg_status %= msg_status_VALUE_MAX;
        // currentDataPoint.valuevolume++;
        // currentDataPoint.valuevolume_delta++;
        // currentDataPoint.valuebattery_percentage++;
        // currentDataPoint.valuemsg_timestamp++;
        // currentDataPoint.valuemsg_id[i++ % 19]++;
        // currentDataPoint.valuemsg_url[i++ % 256]++;
        // system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    }

    // 5 秒更新一次电池电量
    if(times % 5 == 0 ) {
        currentDataPoint.valuebattery_percentage = battery_get_estimate(TYPE_AVERAGE);
        if(currentDataPoint.valuebattery_percentage <= RED_ON_PERCENTAGE && get_battery_state() == BATTERY_NOT_CHARGING)
        {
            tt_led_strip_set_state(TT_LED_STATE_RED);
        }

        if(get_tt_led_last_state() == TT_LED_STATE_RED && currentDataPoint.valuebattery_percentage >= RED_OFF_PERCENTAGE)
        {
            current_led_update();
        }

        battery_check_cb();
    }
    if(times%20 == 0) {
        // if (get_mqtt_is_connected()) {
            printf_cur_flag();
        // }
    }

    // // 每2秒做一次消息推送请求
    // if(times%2 == 0 && get_mqtt_is_connected() && currentDataPoint.valuemsg_req 
    //     && !audio_tone_url_is_playing() && get_need_wait_send_played() == 0) {
    //     printf("Requesting message push at time: %d\n", times);
    //     system_os_post(USER_TASK_PRIO_2, MSG_REQ, 0);
    // }
    
    if(times%3 == 0 && currentDataPoint.valuemsg_req == 0  && get_hall_state() == HALL_STATE_ON && (get_valuestate() != state_VALUE0_close))
    {
        if(get_mqtt_is_connected() && get_socket_client() == NULL)
        {
            SET_WAKEUP_FLAG(true);
            printf("xSemaphoreGive(mqtt_sem) by %s %d", __func__, __LINE__);
            mqtt_sem_give();
        }
    }
    
    // 6 秒更新一次改变的数据点（音量 / 充电状态）
    if(times % 6 == 0)
    {
        system_os_post(USER_TASK_PRIO_2, SIG_UPGRADE_DATA, 0);
    }

    // rb_out_error_cb_1_sec();
}


/**
* Data point initialization function

* In the function to complete the initial user-related data
* @param none
* @return none
* @note The developer can add a data point state initialization value within this function
*/
void ICACHE_FLASH_ATTR userInit(void)
{
    currentDataPoint.valuestate = state_VALUE2_running;
    // gizMemset((uint8_t *)&currentDataPoint, 0, sizeof(dataPoint_t));

 	/** Warning !!! DataPoint Variables Init , Must Within The Data Range **/ 
    /*
            currentDataPoint.valuemsg_req = ;
            currentDataPoint.valuemsg_flag = ;
            currentDataPoint.valuecharge_status = ;
            currentDataPoint.valuemsg_type = ;
            currentDataPoint.valuemsg_status = ;
            currentDataPoint.valuevolume = ;
            currentDataPoint.valuevolume_delta = ;
            currentDataPoint.valuebattery_percentage = ;
    */
}

void set_valuevolume(uint8_t volume)
{
    currentDataPoint.valuevolume = volume;
}
void set_valuevolume_delta(uint8_t volume_delta)
{
    currentDataPoint.valuevolume_delta = volume_delta;
}

void __set_valuestate(const char *func_name, uint32_t line, uint8_t state)
{
    currentDataPoint.valuestate = state;
    printf("%s valuestate: %d by %s %d \n",__func__, currentDataPoint.valuestate, func_name, line);
}

void set_valuecharge_status(uint8_t charge_status)
{
    currentDataPoint.valuecharge_status = charge_status;
}

uint8_t get_valuestate()
{
    if(currentDataPoint.valuestate >= state_VALUE_MAX)
    {
        printf("%s valuestate: error! :%d\n", __func__, currentDataPoint.valuestate);
        return state_VALUE0_close;
    }
    const char *state_str[] = {
        "state_VALUE0_close",
        "state_VALUE1_standby",
        "state_VALUE2_running",
        "state_VALUE_MAX"
    };
    // printf("%s valuestate: %s\n", __func__, state_str[currentDataPoint.valuestate]);
    return currentDataPoint.valuestate;
}


static uint32_t rb_out_sec = 0;
void reset_rb_out_sec()
{
    rb_out_sec = 0;
}
void rb_out_error_cb_1_sec()
{
    if(g_audio_recorder_get_wakeup_state() && !get_voice_sleep_flag() && get_wakeup_flag())
    {
        rb_out_sec++;
    }
    else
    {
        rb_out_sec = 0;
    }
    printf("%s rb_out_sec :%d\n", __func__, rb_out_sec);
    if(rb_out_sec>10)
    {
        audio_sys_get_real_time_stats(1);
        vTaskDelay(pdMS_TO_TICKS(2000));  // 等待日志输出和音频播放
        audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
        vTaskDelay(pdMS_TO_TICKS(2500));  // 等待日志输出和音频播放
        esp_restart();
    }
}