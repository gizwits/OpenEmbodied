#include "wifi_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
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

#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "servo.h"
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>


#define TAG "CustomBoard"

class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    Button* rec_button_ = nullptr;
    PowerSaveTimer* power_save_timer_;
    VbAduioCodec audio_codec;
    Servo servo_;
    bool sleep_flag_ = false;


    static void Uart2Task(void* arg) {
        constexpr int BUF_SIZE = 1024;
        uint8_t* data = (uint8_t*)malloc(BUF_SIZE);
        while (true) {
            int len = uart_read_bytes(UART_NUM_0, data, BUF_SIZE - 1, pdMS_TO_TICKS(1000));
            if (len > 0) {
                data[len] = '\0';
                char* str = (char*)data;
                ESP_LOGI(TAG, "UART2 RX: %s", str);

                // 跳过前导空白字符
                while (*str == '\r' || *str == '\n' || *str == ' ') {
                    ++str;
                }

                // 提取 id
                const char* prefix_ok = "FT+CAROK:";
                const char* prefix_no = "FT+CARNO:";
                char* id = nullptr;
                if (strncmp(str, prefix_ok, strlen(prefix_ok)) == 0) {
                    id = str + strlen(prefix_ok);
                } else if (strncmp(str, prefix_no, strlen(prefix_no)) == 0) {
                    id = str + strlen(prefix_no);
                }
                if (id) {
                    // 去除结尾换行和空格
                    char* end = id + strlen(id) - 1;
                    while (end > id && (*end == '\n' || *end == '\r' || *end == ' ')) {
                        *end = '\0';
                        --end;
                    }
                    ESP_LOGI(TAG, "提取到的ID: %s", id);

                    if (strcmp(id, "88044BCD") == 0) {
                        // 小杰
                        Application::GetInstance().ChangeBot("7509442794864951330", "7468512265134882867");
                    }
                    if (strcmp(id, "3BC25962") == 0) {
                        // 机智云
                        Application::GetInstance().ChangeBot("7486372905413787700", "7468518846874533939");
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        free(data);
        vTaskDelete(NULL);
    }

    void InitializeUart2() {
        uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024 * 2, 0, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, NFC_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)); // RX=GPIO2
        xTaskCreate(Uart2Task, "uart2_task", 2048, NULL, 10, NULL);
    }


    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60 * 1, 60 * 3);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
        });
        power_save_timer_->OnExitSleepMode([this]() {
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            run_sleep_mode(true);
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
        vb6824_shutdown();
        vTaskDelay(pdMS_TO_TICKS(200));
        // 配置唤醒源
        esp_deep_sleep_enable_gpio_wakeup(1ULL << BOOT_BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
        
        esp_deep_sleep_start();
    }

    void InitializeButtons() {

        const int chat_mode = Application::GetInstance().GetChatMode();
        if (chat_mode == 0) {
            rec_button_ = new Button(BUILTIN_REC_BUTTON_GPIO);
            rec_button_->OnPressUp([this]() {
                auto &app = Application::GetInstance();
                app.StopListening();
            });
            rec_button_->OnPressDown([this]() {
                auto &app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StartListening();
            });
        } else {
            boot_button_.OnClick([this]() {
                auto &app = Application::GetInstance();
                app.ToggleChatState();
            });
        }

      
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "Press up");
            if(sleep_flag_){
                run_sleep_mode(false);
            }
        });
        boot_button_.OnPressRepeat([this](uint16_t count) {
            if(count >= 3){
                ResetWifiConfiguration();
            }
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Long press");
            sleep_flag_ = true;
            gpio_set_level(BUILTIN_LED_GPIO, 1);
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_TX_GPIO, CODEC_RX_GPIO), servo_(BUILTIN_SERVO_GPIO, 0){      
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << BUILTIN_LED_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(BUILTIN_LED_GPIO, 0);

        servo_.begin();

        InitializePowerSaveTimer();       
        InitializeButtons();
        InitializeUart2();
        InitializeIot();

        audio_codec.OnWakeUp([this](const std::string& command) {
            ESP_LOGE(TAG, "vb6824 recv cmd: %s", command.c_str());
            if (command == "你好小智" || command.find("小云") != std::string::npos){
                ESP_LOGE(TAG, "vb6824 recv cmd: %d", Application::GetInstance().GetDeviceState());
                // if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                // }
                Application::GetInstance().WakeWordInvoke("你好小智");
            } else if (command == "开始配网") {
                ResetWifiConfiguration();
            }
        });
    }

    virtual Servo* GetServo() override {
        return &servo_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }
};

DECLARE_BOARD(CustomBoard);
