#include "ap_config.h"
#include "wifi_configuration_ap.h"
#include <esp_log.h>
#include <functional>

#define TAG "ApConfig"

// 定义回调函数类型
typedef void (*wifi_connect_success_cb_t)(const char* ssid, const char* password, const char* uid);
typedef void (*wifi_connect_fail_cb_t)(const char* ssid, const char* password, const char* uid);
typedef void (*wifi_ap_connect_cb_t)(const uint8_t* mac, uint8_t aid);  // 新增AP连接回调类型
typedef void (*wifi_ap_disconnect_cb_t)(const uint8_t* mac, uint8_t aid);  // 新增AP断开回调类型

// 保存C回调函数
static wifi_connect_success_cb_t g_success_cb = NULL;
static wifi_connect_fail_cb_t g_fail_cb = NULL;
static wifi_ap_connect_cb_t g_ap_connect_cb = NULL;  // 新增AP连接回调
static wifi_ap_disconnect_cb_t g_ap_disconnect_cb = NULL;  // 新增AP断开回调

// C++回调包装函数，将调用C回调
static void success_callback_wrapper(const std::string& ssid, const std::string& password, const std::string& uid)
{
    if (g_success_cb) {
        g_success_cb(ssid.c_str(), password.c_str(), uid.c_str());
    }
}

static void fail_callback_wrapper(const std::string& ssid, const std::string& password, const std::string& uid)
{
    if (g_fail_cb) {
        g_fail_cb(ssid.c_str(), password.c_str(), uid.c_str());
    }
}

// 新增AP回调包装函数
static void ap_connect_callback_wrapper(const uint8_t* mac, uint8_t aid)
{
    if (g_ap_connect_cb) {
        g_ap_connect_cb(mac, aid);
    }
}

static void ap_disconnect_callback_wrapper(const uint8_t* mac, uint8_t aid)
{
    if (g_ap_disconnect_cb) {
        g_ap_disconnect_cb(mac, aid);
    }
}

extern "C" {

void ap_config_start(const char* ssid_prefix, const char* language)
{
    ap_config_start_with_callbacks(ssid_prefix, language, NULL, NULL, NULL, NULL);
}

void ap_config_start_with_callbacks(const char* ssid_prefix, const char* language, 
                                   wifi_connect_success_cb_t success_cb, 
                                   wifi_connect_fail_cb_t fail_cb,
                                   wifi_ap_connect_cb_t ap_connect_cb,  // 新增参数
                                   wifi_ap_disconnect_cb_t ap_disconnect_cb)  // 新增参数
{
    ESP_LOGI(TAG, "Starting WiFi configuration AP with prefix: %s, language: %s", 
             ssid_prefix ? ssid_prefix : "ESP32", 
             language ? language : "zh-CN");
        
    // 保存回调函数
    g_success_cb = success_cb;
    g_fail_cb = fail_cb;
    g_ap_connect_cb = ap_connect_cb;  // 保存AP连接回调
    g_ap_disconnect_cb = ap_disconnect_cb;  // 保存AP断开回调
    
    auto& ap = WifiConfigurationAp::GetInstance();
    
    // 设置 SSID 前缀
    if (ssid_prefix && strlen(ssid_prefix) > 0) {
        ap.SetSsidPrefix(std::string(ssid_prefix));
    } else {
        ap.SetSsidPrefix("ESP32");
    }
    
    // 设置语言
    if (language && strlen(language) > 0) {
        ap.SetLanguage(std::string(language));
    }
    
    // 启动 WiFi 配置 AP，传入回调函数
    ap.Start(success_callback_wrapper, fail_callback_wrapper);
    
    // 设置AP连接和断开回调
    ap.SetApCallbacks(ap_connect_callback_wrapper, ap_disconnect_callback_wrapper);
    
    ESP_LOGI(TAG, "WiFi configuration AP started");
}

void ap_config_stop()
{
    ESP_LOGI(TAG, "Stopping WiFi configuration AP");
    auto& ap = WifiConfigurationAp::GetInstance();
    ap.Stop();
}

void ap_config_start_smartconfig()
{
    ESP_LOGI(TAG, "Starting SmartConfig");
    auto& ap = WifiConfigurationAp::GetInstance();
    ap.StartSmartConfig();
}

const char* ap_config_get_url()
{
    auto& ap = WifiConfigurationAp::GetInstance();
    static std::string url = ap.GetWebServerUrl();
    return url.c_str();
}

} // extern "C"
