#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "audio_codecs/vb6824_audio_codec.h"
#include "display/eye_toy_single_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "iot/thing_manager.h"
#include "led/single_led.h"
#include "power_save_timer.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include "display/eye_display.h"
#include <esp_lcd_gc9a01.h>
 
#define TAG "CompactWifiBoardLCD"

#if CONFIG_LCD_GC9A01_240X240
    LV_FONT_DECLARE(font_puhui_20_4);   
    LV_FONT_DECLARE(font_awesome_20_4); 
#elif CONFIG_LCD_GC9A01_160X160
    LV_FONT_DECLARE(font_puhui_14_1);   
    LV_FONT_DECLARE(font_awesome_14_1); 
#endif

class CompactWifiBoardLCD : public WifiBoard {
private:
 
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_handle_t lcd_panel = NULL;
    Button boot_button_;
    EyeToySingleDisplay* display_;
    VbAduioCodec audio_codec;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("sleepy");
            #if CONFIG_LCD_GC9A01_160X160
                GetBacklight()->RestoreBrightness();
            #endif
            gpio_set_level(SLEEP_GOIO, 0);

        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
            #if CONFIG_LCD_GC9A01_160X160
                GetBacklight()->RestoreBrightness();
            #endif
            gpio_set_level(SLEEP_GOIO, 1);
        });
        power_save_timer_->OnShutdownRequest([this]() {
            //pmic_->PowerOff();
            //gpio_set_level(SLEEP_GOIO, 0);
        });
        power_save_timer_->SetEnabled(true);
    }

    // 初始化按钮
    void InitializeButtons() {
        // 当boot_button_被点击时，执行以下操作
        boot_button_.OnClick([this]() {
            // 获取应用程序实例
            auto& app = Application::GetInstance();
            // 如果设备状态为kDeviceStateStarting且WifiStation未连接，则重置Wifi配置
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            // 切换聊天状态
            app.ToggleChatState();
        });
         boot_button_.OnPressRepeat([this](uint16_t count) {
            ESP_LOGI(TAG,"boot button down");
            if(count >= 3){
                ESP_LOGI(TAG, "重新配网");
                ResetWifiConfiguration();
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        // thing_manager.AddThing(iot::CreateThing("Lamp"));
    }

    // GC9A01-SPI2初始化-用于显示小智
    void InitializeSpiEye1() {
        const spi_bus_config_t buscfg = {       
            .mosi_io_num = GC9A01_SPI1_LCD_GPIO_MOSI,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = GC9A01_SPI1_LCD_GPIO_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), // 增大传输大小,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI1_NUM, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01-SPI2初始化-用于显示魔眼
    void InitializeGc9a01DisplayEye1() {
        ESP_LOGI(TAG, "Init GC9A01 display1");

        ESP_LOGI(TAG, "Install panel IO1");
        ESP_LOGD(TAG, "Install panel IO1");
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = GC9A01_SPI1_LCD_GPIO_CS,
            .dc_gpio_num = GC9A01_SPI1_LCD_GPIO_DC,
            .spi_mode = 0,
            .pclk_hz = GC9A01_LCD_PIXEL_CLK_HZ,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = GC9A01_LCD_CMD_BITS,
            .lcd_param_bits = GC9A01_LCD_PARAM_BITS,
            
    
        };
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI1_NUM, &io_config, &lcd_io);
        // lcd_io_eye = lcd_io;

        ESP_LOGD(TAG, "Install LCD1 driver");
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GC9A01_SPI1_LCD_GPIO_RST,
            .color_space = GC9A01_LCD_COLOR_SPACE,
            .bits_per_pixel = GC9A01_LCD_BITS_PER_PIXEL,
            
        };
        panel_config.rgb_endian = DISPLAY_RGB_ORDER;
        esp_lcd_new_panel_gc9a01(lcd_io, &panel_config, &lcd_panel);
        // lcd_panel_eye = lcd_panel;

        esp_lcd_panel_reset(lcd_panel);
        esp_lcd_panel_init(lcd_panel);
        esp_lcd_panel_invert_color(lcd_panel, DISPLAY_COLOR_INVERT);
        esp_lcd_panel_disp_on_off(lcd_panel, true);
        display_ = new EyeToySingleDisplay(lcd_io, lcd_panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {
                #if CONFIG_LCD_GC9A01_240X240
                    .text_font = &font_puhui_20_4,
                    .icon_font = &font_awesome_20_4,
                #elif CONFIG_LCD_GC9A01_160X160
                     .text_font = &font_puhui_14_1,
                     .icon_font = &font_awesome_14_1,
                #endif
                .emoji_font = font_emoji_64_init(),
            });
    }

public:
    // CompactWifiBoardLCD() :
    //     boot_button_(BOOT_BUTTON_GPIO) {
      CompactWifiBoardLCD():boot_button_(BOOT_BUTTON_GPIO), audio_codec(CODEC_RX_GPIO, CODEC_TX_GPIO){

        #if CONFIG_LCD_GC9A01_160X160
            gpio_config_t bk_gpio_config = {
                    .pin_bit_mask = 1ULL << GC9A01_SPI1_LCD_GPIO_BL,
                    .mode = GPIO_MODE_OUTPUT,
                };
            ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
            ESP_ERROR_CHECK(gpio_set_level(GC9A01_SPI1_LCD_GPIO_BL, 1));
        #endif

        InitializeSpiEye1();
        InitializeGc9a01DisplayEye1();
        InitializeButtons();
        InitializeIot();

        gpio_set_pull_mode(SLEEP_GOIO, GPIO_PULLUP_ONLY);
        gpio_set_direction(SLEEP_GOIO, GPIO_MODE_OUTPUT);
        gpio_set_level(SLEEP_GOIO, 1);

        InitializePowerSaveTimer();

        auto codec = GetAudioCodec();
        auto volume = codec->output_volume() + 100;
        if (volume > 100) {
            volume = 100;
        }
        codec->SetOutputVolume(volume);
         audio_codec.OnWakeUp([this](const std::string& command) {
            if (command == std::string(vb6824_get_wakeup_word())){
                if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                    Application::GetInstance().WakeWordInvoke("你好小智");
                }
            }else if (command == "开始配网"){
                ResetWifiConfiguration();
            }
        });
        
    }

    // virtual Led* GetLed() override {
    //     static SingleLed led(BUILTIN_LED_GPIO);
    //     return &led;
    // }

   virtual AudioCodec* GetAudioCodec() override {
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }


    #if CONFIG_LCD_GC9A01_160X160
        // 获取背光对象
        virtual Backlight* GetBacklight() override {
            if (GC9A01_SPI1_LCD_GPIO_BL != GPIO_NUM_NC) {
                static PwmBacklight backlight(GC9A01_SPI1_LCD_GPIO_BL, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
                return &backlight;
            }
            return nullptr;
        }
    #endif
};

DECLARE_BOARD(CompactWifiBoardLCD);
