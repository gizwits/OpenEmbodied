#include "wifi_board.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "settings.h"
#include "led/gpio_led.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include <esp_sleep.h>
#include "power_save_timer.h"
#include <driver/rtc_io.h>
#include "driver/gpio.h"
#include <wifi_station.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include "vb6824.h"
#include <esp_wifi.h>

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <vector>
#include <string>
#include "driver/ledc.h"
#include "led_signal.h"
#include "power_manager.h"

#define TAG "CustomBoard"

#define RESET_WIFI_CONFIGURATION_COUNT 3
#define SLEEP_TIME_SEC 60 * 3
// #define SLEEP_TIME_SEC 30
class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    bool sleep_flag_ = false;
    // PowerManager* power_manager_;
    
    // 唤醒词列表
    std::vector<std::string> wake_words_ = {"你好小智", "你好小云", "合养精灵", "嗨小火人"};
    std::vector<std::string> network_config_words_ = {"开始配网"};

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, SLEEP_TIME_SEC, portMAX_DELAY);  // peter mark 休眠时间
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            run_sleep_mode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Shutting down");
            run_sleep_mode(true);
        });
        power_save_timer_->OnShutdownRequest([this]() {
            
        });
        power_save_timer_->SetEnabled(true);
    }

    void run_sleep_mode(bool need_delay = true){
        auto& application = Application::GetInstance();
        if (need_delay) {
            application.Alert("", "", "", Lang::Sounds::P3_SLEEP);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ESP_LOGI(TAG, "Sleep mode");
        }
        application.QuitTalking();

        // 检查不在充电就真休眠
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    void InitializeButtons() {

        const int chat_mode = Application::GetInstance().GetChatMode();
        rec_button_ = new Button(BUILTIN_REC_BUTTON_GPIO);

        if (chat_mode == 0) {
            rec_button_->OnPressUp([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressUp");
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                auto &app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StartListening();
            });
        } else {
            rec_button_->OnPressDown([this]() {
                ESP_LOGI(TAG, "rec_button_.OnPressDown");
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
            boot_button_.OnClick([this]() {
                ESP_LOGI(TAG, "boot_button_.OnClick");
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
        }

        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
        rec_button_->OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "rec_button_.OnPressRepeat: %d", count);
            if(count >= RESET_WIFI_CONFIGURATION_COUNT){
                ResetWifiConfiguration();
            }
        });
    }

    void InitializeLedSignal() {
        LedSignal::GetInstance().MonitorAndUpdateLedState_timer();
    }

    void SetLedBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
    }

    // 检查命令是否在列表中
    bool IsCommandInList(const std::string& command, const std::vector<std::string>& command_list) {
        return std::find(command_list.begin(), command_list.end(), command) != command_list.end();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        ESP_LOGI(TAG, "Initializing IoT components...");
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        ESP_LOGI(TAG, "Added IoT component: Speaker");
        thing_manager.AddThing(iot::CreateThing("Led"));
        ESP_LOGI(TAG, "Added IoT component: Led");
        ESP_LOGI(TAG, "IoT components initialization complete.");
    }

    void InitializePowerManager() {
        PowerManager::GetInstance();
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO){      
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(BUILTIN_LED_GPIO, 0);

        ESP_LOGI(TAG, "Initializing Power Save Timer...");
        InitializePowerSaveTimer();

        ESP_LOGI(TAG, "Initializing Buttons...");

        ESP_LOGI(TAG, "Initializing IoT components...");
        InitializeIot();

        ESP_LOGI(TAG, "Initializing LED Signal...");
        Settings settings("wifi", true);
        auto s_factory_test_mode = settings.GetInt("ft_mode", 0);

        if (s_factory_test_mode == 0) {
            // 不在产测模式才启动，不然有问题
            InitializeButtons();
            InitializeLedSignal();
        }

        ESP_LOGI(TAG, "Initializing Power Manager...");
        InitializePowerManager();
        ESP_LOGI(TAG, "Power Manager initialized.");


        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (IsCommandInList(command, wake_words_)){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (IsCommandInList(command, network_config_words_)) {
                ResetWifiConfiguration();
            }
        });

        PowerManager::GetInstance().CheckBatteryStatusImmediately();
        ESP_LOGI(TAG, "Immediately check the battery level upon startup: %d", PowerManager::GetInstance().GetBatteryLevel());

    }

    virtual void WakeUpPowerSaveTimer() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    };

    bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        level = PowerManager::GetInstance().GetBatteryLevel();
        charging = PowerManager::GetInstance().IsCharging();
        discharging = !charging;
        return true;
    }

    bool IsCharging() override {
        return PowerManager::GetInstance().IsCharging();
    }

    void EnterDeepSleepIfNotCharging() {
        PowerManager::GetInstance().EnterDeepSleepIfNotCharging();
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    void SetPowerSaveTimer(bool enable) {
        power_save_timer_->SetEnabled(enable);
    }

    uint8_t GetBrightness() {
        return LedSignal::GetInstance().GetBrightness();
    }
    
    void SetBrightness(uint8_t brightness) {
        LedSignal::GetInstance().SetBrightness(brightness);
    }

    uint8_t GetDefaultBrightness() {
        return LedSignal::GetInstance().GetDefaultBrightness();
    }

    // 数据点相关方法实现
    const char* GetGizwitsProtocolJson() const override {
        return R"json(
{
  "name": "绿林魔方",
  "packetVersion": "0x00000004",
  "protocolType": "var_len",
  "product_key": "73e57262afa74d6294476c595e42f30f",
  "entities": [
    {
      "display_name": "机智云开发套件",
      "attrs": [
        {
          "display_name": "开关",
          "name": "switch",
          "data_type": "bool",
          "position": {
            "byte_offset": 0,
            "unit": "bit",
            "len": 1,
            "bit_offset": 0
          },
          "type": "status_writable",
          "id": 0,
          "desc": "1"
        },
        {
          "display_name": "唤醒词",
          "name": "wakeup_word",
          "data_type": "bool",
          "position": {
            "byte_offset": 0,
            "unit": "bit",
            "len": 1,
            "bit_offset": 0
          },
          "type": "status_writable",
          "id": 1,
          "desc": ""
        },
        {
          "display_name": "充电状态",
          "name": "charge_status",
          "data_type": "enum",
          "enum": [
            "none",
            " charging",
            "charge_done"
          ],
          "position": {
            "byte_offset": 0,
            "unit": "bit",
            "len": 2,
            "bit_offset": 0
          },
          "type": "status_readonly",
          "id": 2,
          "desc": ""
        },
        {
          "display_name": "提示音语言",
          "name": "alert_tone_language",
          "data_type": "enum",
          "enum": [
            "chinese_simplified",
            "english"
          ],
          "position": {
            "byte_offset": 0,
            "unit": "bit",
            "len": 1,
            "bit_offset": 0
          },
          "type": "status_writable",
          "id": 3,
          "desc": ""
        },
        {
          "display_name": "chat_mode",
          "name": "chat_mode",
          "data_type": "enum",
          "enum": [
            "0",
            "1",
            "2"
          ],
          "position": {
            "byte_offset": 0,
            "unit": "bit",
            "len": 2,
            "bit_offset": 0
          },
          "type": "status_writable",
          "id": 4,
          "desc": "0 按钮\n1 唤醒词\n2 自然对话"
        },
        {
          "display_name": "电量",
          "name": "battery_percentage",
          "data_type": "uint8",
          "position": {
            "byte_offset": 0,
            "unit": "byte",
            "len": 1,
            "bit_offset": 0
          },
          "uint_spec": {
            "addition": 0,
            "max": 100,
            "ratio": 1,
            "min": 0
          },
          "type": "status_readonly",
          "id": 5,
          "desc": ""
        },
        {
          "display_name": "音量",
          "name": "volume_set",
          "data_type": "uint8",
          "position": {
            "byte_offset": 0,
            "unit": "byte",
            "len": 1,
            "bit_offset": 0
          },
          "uint_spec": {
            "addition": 0,
            "max": 100,
            "ratio": 1,
            "min": 0
          },
          "type": "status_writable",
          "id": 6,
          "desc": ""
        },
        {
          "display_name": "rssi",
          "name": "rssi",
          "data_type": "uint8",
          "position": {
            "byte_offset": 0,
            "unit": "byte",
            "len": 1,
            "bit_offset": 0
          },
          "uint_spec": {
            "addition": -100,
            "max": 100,
            "ratio": 1,
            "min": 0
          },
          "type": "status_readonly",
          "id": 7,
          "desc": "无 1"
        },
        {
          "display_name": "亮度",
          "name": "brightness",
          "data_type": "uint8",
          "position": {
            "byte_offset": 0,
            "unit": "byte",
            "len": 1,
            "bit_offset": 0
          },
          "uint_spec": {
            "addition": 0,
            "max": 100,
            "ratio": 1,
            "min": 0
          },
          "type": "status_writable",
          "id": 8,
          "desc": ""
        }
      ],
      "name": "entity0",
      "id": 0
    }
  ]
}
)json";
    }

    size_t GetDataPointCount() const override {
        return 9; // 9个数据点
    }

    bool GetDataPointValue(const std::string& name, int& value) const override {
        if (name == "switch") {
            value = 1; // 开关状态，固定为1
            return true;
        } else if (name == "wakeup_word") {
            value = 1; // 唤醒词状态，固定为1
            return true;
        } else if (name == "charge_status") {
            value = IsCharging() ? 1 : 0; // 充电状态
            return true;
        } else if (name == "alert_tone_language") {
            value = 1; // 提示音语言，固定为中文
            return true;
        } else if (name == "chat_mode") {
            value = Application::GetInstance().GetChatMode();
            return true;
        } else if (name == "battery_percentage") {
            int level = 0;
            bool charging = false, discharging = false;
            GetBatteryLevel(level, charging, discharging);
            value = level;
            return true;
        } else if (name == "volume_set") {
            value = GetAudioCodec()->output_volume();
            return true;
        } else if (name == "rssi") {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                value = 100 - (uint8_t)abs(ap_info.rssi);
            } else {
                value = 0;
            }
            return true;
        } else if (name == "brightness") {
            value = GetBrightness();
            return true;
        }
        return false;
    }

    bool SetDataPointValue(const std::string& name, int value) override {
        if (name == "chat_mode") {
            Application::GetInstance().SetChatMode(value);
            return true;
        } else if (name == "volume_set") {
            GetAudioCodec()->SetOutputVolume(value);
            return true;
        } else if (name == "brightness") {
            SetBrightness(value);
            return true;
        }
        return false;
    }

    void GenerateReportData(uint8_t* buffer, size_t buffer_size, size_t& data_size) override {
        if (buffer_size < 20) {
            data_size = 0;
            return;
        }

        // 固定头部
        buffer[0] = 0x00;
        buffer[1] = 0x00;
        buffer[2] = 0x00;
        buffer[3] = 0x03;
        
        // 命令标识
        buffer[4] = 0x0b;
        buffer[5] = 0x00;
        buffer[6] = 0x00;
        buffer[7] = 0x93;
        
        // 数据长度
        buffer[8] = 0x00;
        buffer[9] = 0x00;
        buffer[10] = 0x00;
        buffer[11] = 0x02;
        
        // 数据类型
        buffer[12] = 0x14;
        buffer[13] = 0x01;
        buffer[14] = 0xff;

        // 状态字节
        uint8_t status = 0;
        status |= (1 << 0); // switch
        status |= (1 << 1); // wakeup_word
        status |= (IsCharging() ? 1 : 0) << 2; // charge_status
        status |= (1 << 4); // alert_tone_language
        status |= (Application::GetInstance().GetChatMode() << 5); // chat_mode
        buffer[15] = status;

        // 电量
        int battery_level = 0;
        bool charging = false, discharging = false;
        GetBatteryLevel(battery_level, charging, discharging);
        buffer[16] = battery_level;

        // 音量
        buffer[17] = GetAudioCodec()->output_volume();

        // RSSI
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            buffer[18] = 100 - (uint8_t)abs(ap_info.rssi);
        } else {
            buffer[18] = 0;
        }

        // 亮度
        buffer[19] = GetBrightness();

        data_size = 20;
    }

    void ProcessDataPointValue(const std::string& name, int value) override {
        ESP_LOGI(TAG, "ProcessDataPointValue: %s = %d", name.c_str(), value);
        SetDataPointValue(name, value);
    }

};

DECLARE_BOARD(CustomBoard);
