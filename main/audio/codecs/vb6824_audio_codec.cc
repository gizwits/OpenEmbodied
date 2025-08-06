#include "vb6824_audio_codec.h"

#include <cstring>
#include <cmath>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "settings.h"
#include <esp_timer.h>

#include "esp_log.h"

#include "application.h"
#include "system_info.h"
#include "wifi_station.h"
#include "mbedtls/md5.h"
#include <iomanip>
#include <sstream>

#include "vb6824.h"

static const char *TAG = "VbAduioCodec";

#define VB_PLAY_SAMPLE_RATE     16 * 1000
#define VB_RECO_SAMPLE_RATE     16 * 1000

void VbAduioCodec::WakeUp(std::string command) {
    if (on_wake_up_) {
        on_wake_up_(command);
    }
}

void VbAduioCodec::OnWakeUp(std::function<void(std::string)> callback) {
    on_wake_up_ = callback;
}

void VbAduioCodec::OtaEvent(vb6824_evt_t event_id, uint32_t data) {
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    ESP_LOGW(TAG, "event_id: %d %ld", event_id, data);
    if (event_id == VB6824_EVT_OTA_ENTER) {
        if (data == 0 && esp_timer_get_time() > 20 * 1000 * 1000)
        {
            return; 
        }
        OtaStart();
    }else if (event_id == VB6824_EVT_OTA_START) {
        ESP_LOGI(TAG, "OTA START");
        auto& app = Application::GetInstance();
        app.ReleaseDecoder();
    }
#else
    ESP_LOGW(TAG, "not support ota event");
#endif
}

std::string VbAduioCodec::GenDevCode() {

    auto mac = SystemInfo::GetMacAddress();
    std::string last_four_digits;
    if (!mac.empty()) {
        unsigned char md5_result[16]; // MD5 produces a 16-byte hash
        mbedtls_md5(reinterpret_cast<const unsigned char*>(mac.c_str()), mac.size(), md5_result);
        uint16_t value =  (((md5_result[14]) << 8) | md5_result[15]) & 0xFFFF; 
        std::ostringstream oss;
        oss << value % 10000;
        last_four_digits = oss.str();
        ESP_LOGW(TAG, "last_four_digits: %s", last_four_digits.c_str());
    }
    if (last_four_digits.empty()) {
        last_four_digits = "0000"; // 如果无法生成，返回默认值
    }
    
    return last_four_digits;
}

int VbAduioCodec::OtaStart(uint8_t mode) {
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    if (vb6824_is_support_ota()==false) {
        ESP_LOGE(TAG, "ota not support");
        return OTA_ERR_NOT_SUPPORT;
    }

    auto& app = Application::GetInstance();
    auto& wifi_station = WifiStation::GetInstance();
    const std::string ip = wifi_station.GetIpAddress();
    std::string code = GenDevCode();
    if(!wifi_station.IsConnected() || app.GetDeviceState() == kDeviceStateActivating) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app.Schedule([this]() {
            this->OtaStart(1);
        });
        return OTA_OK;
    }

    if (jl_ws_is_start() == 1)
    {
        app.ShowOtaInfo(code, ip);
        return OTA_ERR_IN_OTA_MODE;
    }
    
    ESP_LOGI(TAG, "升级模式");
    SetOutputVolume(100);
    app.ShowOtaInfo(code, ip);
    jl_ws_start((char*)code.c_str());
    return OTA_OK;
#else
    return OTA_ERR_NOT_SUPPORT;
#endif
}

bool VbAduioCodec::InOtaMode(bool reShowIfInOta) {
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    auto &app = Application::GetInstance();
    std::string code = GenDevCode();
    const std::string ip = WifiStation::GetInstance().GetIpAddress();
    if (jl_ws_is_start() == 1) {
        if (reShowIfInOta)
        {
            app.ShowOtaInfo(code, ip);
        }
        return true;
    }
#endif
    return false;
}


VbAduioCodec::VbAduioCodec(gpio_num_t tx, gpio_num_t rx) {

    input_sample_rate_ = VB_RECO_SAMPLE_RATE;
    output_sample_rate_ = VB_RECO_SAMPLE_RATE;

    vb6824_init(tx, rx);

    vb6824_register_voice_command_cb([](char *command, uint16_t len, void *arg){
        auto this_ = (VbAduioCodec*)arg;
        this_->WakeUp(command);
    }, this);

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    vb6824_register_event_cb([](vb6824_evt_t event_id, uint32_t data, void *arg){
        auto this_ = (VbAduioCodec*)arg;
        this_->OtaEvent(event_id, data);
    }, this);
#endif

}

void VbAduioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }
    
    ESP_LOGI(TAG, "VB6824 starting with output_volume=%d", output_volume_);
    EnableInput(true);
    EnableOutput(true);
}

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
bool VbAduioCodec::InputData(std::vector<uint8_t>& opus) {
    // VB6824 每次返回 40 字节的 OPUS 数据
    opus.resize(40);
    int samples = Read((uint8_t *)opus.data(), opus.size());
    ESP_LOGD(TAG, "VB6824 InputData: samples=%d, input_enabled=%s", samples, input_enabled_ ? "true" : "false");
    if (samples > 0) {
        return true;
    }
    return false;
}
#endif

void VbAduioCodec::SetOutputVolume(int volume){
    vb6824_audio_set_output_volume(volume);
    AudioCodec::SetOutputVolume(volume);
}

void VbAduioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    vb6824_audio_enable_input(enable);
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void VbAduioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    vb6824_audio_enable_output(enable);
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

int VbAduioCodec::Read(int16_t* dest, int samples) {
    int read_len = vb6824_audio_read((uint8_t *)dest, 2 * samples);
    return read_len / 2;
}

#ifdef CONFIG_USE_AUDIO_CODEC_ENCODE_OPUS
int VbAduioCodec::Read(uint8_t* dest, int samples) {
    int read_len = vb6824_audio_read((uint8_t *)dest, samples);
    ESP_LOGD(TAG, "VB6824 Read OPUS: requested=%d, actual=%d", samples, read_len);
    return read_len;
}
#endif

int VbAduioCodec::Write(const int16_t* data, int samples) {
    if(frist_volume_is_set == false){
        frist_volume_is_set = true;
        SetOutputVolume(output_volume_);
    }
    vb6824_audio_write((uint8_t *)data, 2 * samples);
    return samples;
}

#ifdef CONFIG_USE_AUDIO_CODEC_DECODE_OPUS
int VbAduioCodec::Write(uint8_t* opus, int samples) {
    if(frist_volume_is_set == false){
        frist_volume_is_set = true;
        SetOutputVolume(output_volume_);
    }
    vb6824_audio_write((uint8_t *)data, samples);
    return samples;
}

bool ConfigDecode(int sample_rate, int channels, int duration_ms) {
    input_sample_rate_ = sample_rate;
    input_channels_ = channels;
    duration_ms_ = duration_ms;
    return true;
}
#endif
