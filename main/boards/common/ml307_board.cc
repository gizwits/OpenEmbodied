#include "ml307_board.h"

#include "application.h"
#include "display.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <opus_encoder.h>

static const char *TAG = "Ml307Board";

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, uart_port_t uart_num) : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), uart_num_(uart_num) {
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::StartNetwork() {
    ESP_LOGI(TAG, "开始启动4G网络连接...");
    auto& application = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DETECTING_MODULE);
    ESP_LOGI(TAG, "开始检测ML307模块...");

    while (true) {
        ESP_LOGI(TAG, "尝试检测ML307模块...");
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 921600, uart_num_);
        if (modem_ != nullptr) {
            ESP_LOGI(TAG, "成功检测到ML307模块");
            break;
        }
        ESP_LOGW(TAG, "未检测到ML307模块，1秒后重试...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "设置网络状态变化回调...");
    modem_->OnNetworkStateChanged([this, &application](bool network_ready) {
        if (network_ready) {
            //ESP_LOGI(TAG, "Network is ready");
            ESP_LOGI(TAG, "网络已准备就绪");
        } else {
            //ESP_LOGE(TAG, "Network is down");
            ESP_LOGE(TAG, "网络连接断开");
            auto device_state = application.GetDeviceState();
            if (device_state == kDeviceStateListening || device_state == kDeviceStateSpeaking) {
                application.Schedule([this, &application]() {
                    application.SetDeviceState(kDeviceStateIdle);
                });
            }
        }
    });

    // Wait for network ready
    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
    ESP_LOGI(TAG, "等待网络注册...");
    while (true) {
        ESP_LOGI(TAG, "检查网络注册状态...");
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::ErrorInsertPin) {
            ESP_LOGE(TAG, "SIM卡PIN错误");
            application.Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "sad", Lang::Sounds::P3_ERR_PIN);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            ESP_LOGE(TAG, "网络注册被拒绝");
            application.Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "sad", Lang::Sounds::P3_ERR_REG);
        } else {
            ESP_LOGI(TAG, "网络注册成功");
            break;
        }
        ESP_LOGI(TAG, "10秒后重试网络注册...");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Print the ML307 modem information
    ESP_LOGI(TAG, "获取模块信息...");
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    // ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    // ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());
    // ESP_LOGI(TAG, "ML307 ICCID: %s", iccid.c_str());
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());
    ESP_LOGI(TAG, "ML307 ICCID: %s", iccid.c_str());
    ESP_LOGI(TAG, "4G网络连接完成");
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        ESP_LOGW(TAG, "网络未就绪或modem为空");
        return FONT_AWESOME_SIGNAL_OFF;
    }
    int csq = modem_->GetCsq();
    ESP_LOGI(TAG, "当前信号质量(CSQ): %d", csq);
    if (csq == -1) {
        ESP_LOGW(TAG, "无法获取信号质量");
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 14) {
        ESP_LOGI(TAG, "信号强度: 很弱");
        return FONT_AWESOME_SIGNAL_1;
    } else if (csq >= 15 && csq <= 19) {
        ESP_LOGI(TAG, "信号强度: 弱");
        return FONT_AWESOME_SIGNAL_2;
    } else if (csq >= 20 && csq <= 24) {
        ESP_LOGI(TAG, "信号强度: 中等");
        return FONT_AWESOME_SIGNAL_3;
    } else if (csq >= 25 && csq <= 31) {
        ESP_LOGI(TAG, "信号强度: 强");
        return FONT_AWESOME_SIGNAL_4;
    }

    ESP_LOGW(TAG, "无效的信号质量值: %d", csq);
    //ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    // Set the board type for OTA
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveMode(bool enabled) {
    // TODO: Implement power save mode for ML307
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
