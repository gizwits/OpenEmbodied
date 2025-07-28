#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include "led/circular_strip.h"
#include <esp_timer.h>

#define TAG "GizwitsDev"


// 电压-电量查表
typedef struct {
    uint16_t voltage; // mV
    uint8_t soc;      // 百分比
} VoltageSocPair;

const VoltageSocPair dischargeCurve[] = {
    {4163, 100}, {4098, 95}, {4039, 90}, {3983, 85}, {3930, 80}, {3878, 75}, {3829, 70}, {3784, 65}, {3745, 60}, {3710, 55},
    {3668, 50},  {3645, 45}, {3629, 40}, {3615, 35}, {3600, 30}, {3583, 25}, {3554, 20}, {3520, 15}, {3476, 10}, {3439, 5}, {3395, 0}
};

static uint8_t estimate_soc(uint16_t voltage, const VoltageSocPair *soc_pairs, int voltage_soc_pairs_length)
{
    if (soc_pairs == NULL) return 0;
    uint16_t closest_voltage = soc_pairs[0].voltage;
    uint8_t closest_soc = soc_pairs[0].soc;
    uint16_t min_diff = abs((int)voltage - (int)closest_voltage);
    for (int i = 1; i < voltage_soc_pairs_length; i++) {
        uint16_t diff = abs((int)voltage - (int)soc_pairs[i].voltage);
        if (diff < min_diff) {
            min_diff = diff;
            closest_voltage = soc_pairs[i].voltage;
            closest_soc = soc_pairs[i].soc;
        }
    }
    return closest_soc;
}

class GizwitsDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    adc_oneshot_unit_handle_t adc2_handle_;  // ADC句柄

    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button rec_button_;
    bool need_power_off_ = false;
    
    // 按钮事件队列
    QueueHandle_t button_event_queue_;

    int64_t rec_last_click_time_ = 0;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    void InitializeButtons() {
        static int first_level = gpio_get_level(BOOT_BUTTON_GPIO);
        boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG, "boot_button_.OnPressRepeat");
            if(count >= 3){
                ResetWifiConfiguration();
            } else {
                // Application::GetInstance().ToggleChatState();
            }
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "boot_button_.OnLongPress");
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateIdle);

            if (first_level ==0) {
                first_level = 1;
            } else {
                need_power_off_ = true;
            }
        });
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "boot_button_.OnPressUp");
            if (need_power_off_) {
                xTaskCreate([](void* arg) {
                    Application::GetInstance().SetDeviceState(kDeviceStateIdle);
                    gpio_set_level(POWER_HOLD_GPIO, 0);
                }, "power_off_task", 2048, NULL, 10, NULL);
            }
        });

        auto chat_mode = Application::GetInstance().GetChatMode();
        ESP_LOGI(TAG, "chat_mode: %d", chat_mode);

        rec_button_.OnClick([this]() {
            ESP_LOGI(TAG, "rec_button_.OnClick");
            Application::GetInstance().StartListening();
        });
        rec_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "rec_button_.OnPressDown");
        });
        rec_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "rec_button_.OnPressUp");
        });
        
        volume_up_button_.OnClick([this]() {
            ESP_LOGI(TAG, "volume_up_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            codec->SetOutputVolume(volume);
        });

        volume_down_button_.OnClick([this]() {
            ESP_LOGI(TAG, "volume_down_button_.OnClick");
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            codec->SetOutputVolume(volume);
        });

    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

    // 初始化ADC
    void InitializeAdc() {
        adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_2 };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc2_handle_));

        adc_oneshot_chan_cfg_t config = { .atten = BAT_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_12 };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc2_handle_, BAT_ADC_CHANNEL, &config));
        
        ESP_LOGI(TAG, "ADC initialized for battery voltage detection");
    }

public:
    GizwitsDevBoard() : boot_button_(BOOT_BUTTON_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
    rec_button_(REC_BUTTON_GPIO) {
        InitializeButtons();
        InitializeGpio(POWER_HOLD_GPIO, true);
        InitializeChargingGpio();
        InitializeI2c();
        InitializeIot();
        InitializeAdc();  // 初始化ADC
    }

    ~GizwitsDevBoard() {
        // 清理ADC资源
        if (adc2_handle_) {
            adc_oneshot_del_unit(adc2_handle_);
        }
    }


    void InitializeGpio(gpio_num_t gpio_num_, bool output = false) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        if (output) {
            gpio_set_level(gpio_num_, 1);
        } else {
            gpio_set_level(gpio_num_, 0);
        }
    }


    void InitializeChargingGpio() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << CHARGING_PIN) | (1ULL << STANDBY_PIN),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // 需要上拉，因为这些引脚是开漏输出
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ESP_ERROR_CHECK(gpio_config(&io_conf));
    }

    bool isCharging() {
        int chrg = gpio_get_level(CHARGING_PIN);
        int standby = gpio_get_level(STANDBY_PIN);
        ESP_LOGI(TAG, "chrg: %d, standby: %d", chrg, standby);
        return chrg == 0 || standby == 0;
    }
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        // 1. 读取ADC原始值（多次采样取平均）
        bool is_charging = isCharging();
        
        const int sample_count = 5;  // 采样5次取平均
        int total_raw = 0;
        esp_err_t ret = ESP_OK;
        
        for (int i = 0; i < sample_count; i++) {
            int raw = 0;
            ret = adc_oneshot_read(adc2_handle_, BAT_ADC_CHANNEL, &raw);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ADC2 oneshot read failed: %d", ret);
                level = 0;
                charging = false;
                discharging = false;
                return false;
            }
            total_raw += raw;
        }
        
        int raw = total_raw / sample_count;  // 取平均值

        // 2. 转换为电压（假设3.6V满量程，12位ADC，分压比2:1）
        // 公式: 电压(mV) = raw / 4095.0 * 3600 * 2
        float voltage = raw * 3600.0f / 4095.0f * 2.0f;
        uint16_t voltage_mv = (uint16_t)voltage;
        ESP_LOGI(TAG, "ADC raw: %d, voltage: %d mV", raw, voltage_mv);

        charging = is_charging;
        discharging = !charging;

        // 4. 估算电量
        if (charging) {
            // 线性估算
            if (voltage_mv >= 4163) {
                level = 100;
            } else if (voltage_mv <= 3395) {
                level = 0;
            } else {
                level = ((voltage_mv - 3395) * 100) / (4163 - 3395);
            }
        } else {
            // 查表法
            level = estimate_soc(voltage_mv, dischargeCurve, sizeof(dischargeCurve)/sizeof(dischargeCurve[0]));
        }

        // 限制范围
        if (level > 100) level = 100;
        if (level < 0) level = 0;

        ESP_LOGI(TAG, "level: %d, charging: %d, discharging: %d", level, charging, discharging);

        return true;
    }


    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 4);
        return &led;
    }
    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, 
            I2C_NUM_1, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_GPIO_PA, 
            AUDIO_CODEC_ES8311_ADDR, 
            false);

        return &audio_codec;
    }

};

DECLARE_BOARD(GizwitsDevBoard);
