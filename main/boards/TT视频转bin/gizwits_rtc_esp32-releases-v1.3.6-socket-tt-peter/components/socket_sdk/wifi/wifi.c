#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "board/gpio.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "wifi.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "storage/storage.h"
#include "sdk_api.h"
#include "gizwits_protocol.h"
#include "tt_ledc.h"
#include "mqtt.h"
#include "audio_processor.h"

static const char *TAG = "WiFi";
static TimerHandle_t wifi_reconnect_timer = NULL;
static wifi_connected_callback_t wifi_connected_cb = NULL;
static wifi_disconnected_callback_t wifi_disconnected_cb = NULL;

static EventGroupHandle_t s_wifi_event_group;
static uint8_t onboarding_on = 0;// 设备是否在等待配网状态
uint8_t get_onboarding_on(void)
{
    ESP_LOGI(TAG, "%s %d", __func__, onboarding_on);
    return onboarding_on;
}

void __set_onboarding_on( const char *funN, int32_t line, uint8_t value)
{
    ESP_LOGI(TAG, "%s %d, by %s %d", __func__, value, funN, line);
    onboarding_on = value;
}

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_READY_BIT     BIT2

static int s_retry_num = 0;
#define MAXIMUM_RETRY  5

bool wifi_is_connected = false;
bool wifi_is_connecting = false;
char wifi_ssid[WIFI_SSID_MAX_LEN + 1] = {0};  // 当前连接的WiFi SSID
char wifi_password[WIFI_PASSWORD_MAX_LEN + 1] = {0};  // 当前连接的WiFi密码

// 使用二值信号量来表示WiFi连接状态
static SemaphoreHandle_t wifi_connect_sem = NULL;

// 添加一个标志来跟踪是否已经因为WiFi问题重启过
static bool already_restarted_for_wifi = false;

#define WIFI_SSID_KEY "ssid"
#define WIFI_PASS_KEY "password"
#define WIFI_RESTART_FLAG_KEY "wifi_restart"

static bool is_manual_disconnect = false;  // 标记是否是主动断开
static uint8_t error_played = 0;
static int reconnect_count = 0;           // 重连计数器
#define MAX_RECONNECT_ATTEMPTS 3         // 最大重连次数

void reset_wifi_restart_flag(void)
{
    // clear_restart_flag();
    error_played = 0;
    set_ws_error_played(0);
    reconnect_count = 0;
}

bool get_esp_wifi_is_connected(void) {
    return wifi_is_connected;
}

// 从NVS加载重启标志
static bool load_restart_flag(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }
    
    uint8_t flag = 0;
    err = nvs_get_u8(nvs_handle, WIFI_RESTART_FLAG_KEY, &flag);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && flag == 1) {
        return true;
    }
    return false;
}

// 保存重启标志到NVS
static void save_restart_flag(bool flag) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_u8(nvs_handle, WIFI_RESTART_FLAG_KEY, flag ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error setting restart flag: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
}
void clear_restart_flag(void)
{
    save_restart_flag(false);
}

esp_err_t wifi_save_config(const char *ssid, const char *password)
{
    return storage_save_wifi_config(ssid, password);
}

esp_err_t wifi_load_config(char *ssid, size_t ssid_size, 
                          char *password, size_t password_size)
{
    if (!ssid || !password || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool success = storage_load_wifi_config(ssid, password);
    if (!success) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "wifi_load_config SSID: %s", ssid);

    return ESP_OK;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{

    if (get_valuestate() == state_VALUE0_close) {
        ESP_LOGI(TAG, "%s MagicI is close, exiting task", __func__);
        return;
    }
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA ready");
        reset_wifi_restart_flag();
        xEventGroupSetBits(s_wifi_event_group, WIFI_READY_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        ESP_LOGI(TAG, "WiFi STA stopped");
        reset_wifi_restart_flag();
        xEventGroupClearBits(s_wifi_event_group, WIFI_READY_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_is_connected = false;
        wifi_is_connecting = false;
        
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
        if (!is_manual_disconnect) {  // 只有非主动断开才进行重连
            reconnect_count++;
            ESP_LOGI(TAG, "WiFi disconnected, attempt to reconnect (%d/%d)", 
                     reconnect_count, MAX_RECONNECT_ATTEMPTS);
            if(reconnect_count == 1 && !get_onboarding_on())// 抛开不是由于进配网导致的断开
            {
                vTaskDelay(pdMS_TO_TICKS(2000));  // 等待日志输出和音频播放
                audio_tone_play(0, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
                vTaskDelay(pdMS_TO_TICKS(2500));  // 等待日志输出和音频播放
            }
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            
            if (reconnect_count >= MAX_RECONNECT_ATTEMPTS) {
                ESP_LOGE(TAG, "Max reconnection attempts reached");
                
                // 检查是否已经重启过
                already_restarted_for_wifi = load_restart_flag();
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
                
                if (!already_restarted_for_wifi) {
                    if (factory_test_is_enabled()) {
                        ESP_LOGE(TAG, "factory test mode is enabled, won't restart again");
                        // 继续尝试重连，但不再重启
                        reconnect_count = MAX_RECONNECT_ATTEMPTS - 1;
                    } else {
                        ESP_LOGE(TAG, "First WiFi failure, will restart system...");
                        // 设置重启标志
                        save_restart_flag(true);
                        user_event_notify(USER_EVENT_WIFI_RECONNECT_FAILED);
                        vTaskDelay(pdMS_TO_TICKS(6000));  // 等待日志输出和音频播放
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
                        esp_restart();  // 重启系统
                    }
                } else {
                    ESP_LOGE(TAG, "Already restarted once for WiFi, won't restart again");
                    // 继续尝试重连，但不再重启
                    reconnect_count = MAX_RECONNECT_ATTEMPTS - 1;
                    if(!error_played)
                    {
                        audio_tone_play(0, 0, "spiffs://spiffs/converted_connectFail_2.mp3");
                        error_played = 1;
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
                    }
                }
            }
            
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
            // 尝试重新连接
            if (wifi_ssid[0] != '\0') { 
                esp_wifi_connect();  // 重新连接
            }
        } else {
            memset(wifi_ssid, 0, sizeof(wifi_ssid));  // 主动断开时清除SSID
        }
        
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
        xSemaphoreGive(wifi_connect_sem);
        if (wifi_disconnected_cb) {
            wifi_disconnected_cb();
        }
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_is_connected = true;
        wifi_is_connecting = false;
        reconnect_count = 0;  // 连接成功后重置重连计数器
        if(get_room_info_request_id() == 0)
        {
            set_room_info_request_id(xTaskGetTickCount()+battery_get_voltage());
        }

        
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
        // 连接成功，清除重启标志
        if (load_restart_flag()) {
            ESP_LOGI(TAG, "WiFi connected successfully, clearing restart flag");
            save_restart_flag(false);
        }

        user_event_notify(USER_EVENT_WIFI_RECONNECTED);
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);

        
        xSemaphoreGive(wifi_connect_sem);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        reset_wifi_restart_flag();
    }
    ESP_LOGI(TAG, "%s %d", __func__, __LINE__);
}

void init_wifi_station(void)
{
    
    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());


    // 创建 WiFi 事件组
    s_wifi_event_group = xEventGroupCreate();

    // 创建默认的 wifi station 接口
    esp_netif_create_default_wifi_sta();

    // 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &event_handler,
                                                      NULL,
                                                      NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &event_handler,
                                                      NULL,
                                                      NULL));

    // 设置 WiFi 模式为 station 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // 启动 WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
}

void ap_wifi_connect_success_callback(const char* ssid, const char* password, const char* uid) {
    ESP_LOGI(TAG, "WiFi connected successfully!");
    ESP_LOGI(TAG, "SSID: %s", ssid);
    ESP_LOGI(TAG, "Password: %s", password);
    if (uid && strlen(uid) > 0) {
        ESP_LOGI(TAG, "UID: %s", uid);
    }

    // 保存数据
    wifi_save_config(ssid, password);
    storage_save_uid(uid);

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

void ap_wifi_connect_fail_callback(const char* ssid, const char* password, const char* uid) {
    ESP_LOGW(TAG, "WiFi connection failed!");
    ESP_LOGW(TAG, "Failed SSID: %s", ssid);
    ESP_LOGW(TAG, "Failed Password: %s", password);
    if (uid && strlen(uid) > 0) {
        ESP_LOGW(TAG, "Failed UID: %s", uid);
    }
}

// 新增AP连接和断开回调函数
void ap_wifi_ap_connect_callback(const uint8_t* mac, uint8_t aid) {
    ESP_LOGI(TAG, "Device connected to AP: MAC=%02X:%02X:%02X:%02X:%02X:%02X, AID=%d",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], aid);
    
    // 停止蓝牙广播
    ble_stop_advertising();
    ESP_LOGI(TAG, "Bluetooth advertising stopped due to AP connection");
}

void ap_wifi_ap_disconnect_callback(const uint8_t* mac, uint8_t aid) {
    ESP_LOGI(TAG, "Device disconnected from AP: MAC=%02X:%02X:%02X:%02X:%02X:%02X, AID=%d",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], aid);
    
    // 重新开始蓝牙广播
    ble_start_advertising();
    ESP_LOGI(TAG, "Bluetooth advertising resumed after AP disconnection");
}

esp_err_t wifi_init(wifi_connected_callback_t connect_cb, 
                    wifi_disconnected_callback_t disconnect_cb)
{
    // 加载重启标志
    already_restarted_for_wifi = load_restart_flag();
    if (already_restarted_for_wifi) {
        ESP_LOGW(TAG, "Device has already restarted due to WiFi issues");
    }
    
    esp_err_t ret = ESP_OK;
    wifi_connected_cb = connect_cb;
    wifi_disconnected_cb = disconnect_cb;
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 尝试加载并连接保存的WiFi
    char ssid[33] = {0};
    char password[65] = {0};
    bool has_saved_wifi = false;
    
    bool onboarding_flag = false;
    storage_load_onboarding_flag(&onboarding_flag);

    
    if (!onboarding_flag && wifi_load_config(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        // 直接初始化sta 然后连接路由
        init_wifi_station();

        has_saved_wifi = true;
        ESP_LOGI(TAG, "Found saved WiFi config, trying to connect...");
        wifi_connect(ssid, password);
        set_onboarding_on(0);
    } else {
        // 进入ap 配网状态
        ap_config_start_with_callbacks("XPG-GAgent", "en-US", 
                                        ap_wifi_connect_success_callback, 
                                        ap_wifi_connect_fail_callback,
                                        ap_wifi_ap_connect_callback,
                                        ap_wifi_ap_disconnect_callback);
        // 初始化蓝牙配网
        ble_init(onboarding_flag);
        user_event_notify(USER_EVENT_WIFI_INIT);
        storage_save_onboarding_flag(false);
        
        ESP_LOGI(TAG, "onboarding_on = 1");
        set_onboarding_on(1);
        tt_led_strip_set_state(TT_LED_STATE_ORANGE);

    }

    if (!has_saved_wifi) {
        ESP_LOGI(TAG, "No saved WiFi config found");
    }

    ESP_LOGI(TAG, "wifi_init finished.");
    return ret;
}

esp_err_t wifi_connect_and_switch_to_sta(const char *ssid, const char *password)
{
    // 先停止当前的 WiFi 模式
    ESP_ERROR_CHECK(esp_wifi_stop());
    
    // 切换到 station 模式
    init_wifi_station();
    
    // 等待一小段时间确保模式切换完成
    vTaskDelay(pdMS_TO_TICKS(100));
    
    set_onboarding_on(0);

    // 连接到指定的 WiFi
    return wifi_connect(ssid, password);
}
esp_err_t wifi_connect(const char *ssid, const char *password)
{

    // 如果 ssid 是当前ssid，且已连接，则直接返回
    if (wifi_is_connected && strcmp(ssid, wifi_ssid) == 0) {
        ESP_LOGI(TAG, "Already connected to SSID: %s", ssid);
        return ESP_OK;
    }

    // 延迟创建信号量
    if (wifi_connect_sem == NULL) {
        wifi_connect_sem = xSemaphoreCreateBinary();
        xSemaphoreGive(wifi_connect_sem);  // 初始状态为可用
    }

    // 检查是否可以开始连接
    if (xSemaphoreTake(wifi_connect_sem, 0) != pdTRUE) {
        ESP_LOGW(TAG, "WiFi is busy connecting");
        return ESP_ERR_WIFI_CONNECTING;
    }

    // 如果 wifi 已经连接，先断开
    if (wifi_is_connected) {
        ESP_LOGI(TAG, "WiFi is already connected, disconnecting...");
        wifi_disconnect();
        while (wifi_is_connected) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    wifi_is_connecting = true;

    // 等待 STA 接口准备就绪
    EventBits_t staBits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_READY_BIT,  // 新增一个状态位
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(5000));  // 等待最多5秒

    if ((staBits & WIFI_READY_BIT) == 0) {
        ESP_LOGE(TAG, "WiFi STA not ready");
        return ESP_ERR_WIFI_NOT_INIT;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // 复制 SSID 和密码
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    // 保存SSID
    strncpy(wifi_ssid, ssid, WIFI_SSID_MAX_LEN);
    // 保存密码
    strncpy(wifi_password, password, WIFI_PASSWORD_MAX_LEN);
    wifi_ssid[WIFI_SSID_MAX_LEN] = '\0';  // 确保字符串结束
    wifi_password[WIFI_PASSWORD_MAX_LEN] = '\0';  // 确保字符串结束
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
    set_onboarding_on(0);
    tt_led_strip_set_state(TT_LED_STATE_OFF);

    ESP_LOGI(TAG, "wifi_connect finished.");

    /* 等待连接结果 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() 返回时检查连接结果 */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ssid, password);
        user_event_notify(USER_EVENT_WIFI_CONNECTED);

        wifi_save_config(ssid, password);
        if (wifi_connected_cb) {
            ESP_LOGI(TAG, "run wifi_connected_cb");
            wifi_connected_cb();
        }
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ssid, password);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t wifi_disconnect(void)
{
    is_manual_disconnect = true;  // 设置主动断开标志
    wifi_is_connecting = false;
    memset(wifi_ssid, 0, sizeof(wifi_ssid));
    // set_room_info_req_success(0);

    if (get_valuestate() == state_VALUE0_close) {
        ESP_LOGI(TAG, "%s MagicI is close, exiting task", __func__);
        return ESP_OK;
    }
    
    if (wifi_connect_sem != NULL) {
        xSemaphoreGive(wifi_connect_sem);
    }
    
    if (wifi_disconnected_cb) {
        wifi_disconnected_cb();
    }
    
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(1000));
            
    is_manual_disconnect = false;  // 重置标志
    user_event_notify(USER_EVENT_WIFI_DISCONNECTED);
    return ESP_OK;
}

// 添加一个新的函数用于完全停止WiFi
esp_err_t app_wifi_deinit(void)
{
    // 确保先断开连接
    // wifi_disconnect();
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // 这里会crash
    // if (s_wifi_event_group) {
    //     vEventGroupDelete(s_wifi_event_group);
    //     s_wifi_event_group = NULL;
    // }
    return ESP_OK;
}



esp_err_t wifi_clear_config(void)
{

}
