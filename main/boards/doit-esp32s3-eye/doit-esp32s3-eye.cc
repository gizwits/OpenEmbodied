#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/eye_toy_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include "esp_lvgl_port.h"
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>

#include "esp_random.h"
#include "display/eye_display.h"

#define TAG "XiaoZhiEyeBoard"

LV_FONT_DECLARE(font_puhui_20_4);   //表示名为 puhui 的字体,是一个常规字体，用于显示文本内容，大小为 14
LV_FONT_DECLARE(font_awesome_20_4); //表示名为 font_awesome 的字体,是一个图标字体库，用于显示各种图标，大小为 14


class XiaoZhiEyeBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    EyeToyDisplay* display_;
     /* LCD IO and panel */
    esp_lcd_panel_io_handle_t lcd_io1 = NULL;
    esp_lcd_panel_handle_t lcd_panel1 = NULL;
    esp_lcd_panel_io_handle_t lcd_io2 = NULL;
    esp_lcd_panel_handle_t lcd_panel2 = NULL;

    SemaphoreHandle_t lcd_mutex = NULL;
    
    // 三击相关变量
    int boot_button_click_count_ = 0;
    TimerHandle_t boot_button_timer_ = nullptr;
    static constexpr int kTripleClickWindowMs = 1000; // 1秒内三击

    // 三击定时器回调
    static void BootButtonTimerCallback(TimerHandle_t xTimer) {
        auto self = static_cast<XiaoZhiEyeBoard*>(pvTimerGetTimerID(xTimer));
        self->boot_button_click_count_ = 0;
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

     //物联网初始化，添加对AI可见设备
     void initializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        //thing_manager.AddThing(iot::CreateThing("Screen"));
    }

    void InitializeButtons() {
        // 创建三击定时器
        if (!boot_button_timer_) {
            boot_button_timer_ = xTimerCreate("boot_btn_timer", pdMS_TO_TICKS(kTripleClickWindowMs), pdFALSE, this, BootButtonTimerCallback);
        }
        // boot_button_.OnLongPress([this]() {
        //     esp_restart();
        // });
        boot_button_.OnClick([this]() {
            boot_button_click_count_++;
            ESP_LOGI(TAG, "Boot button clicked %d times", boot_button_click_count_);
            if (boot_button_click_count_ == 1) {
                // 启动定时器
                xTimerStart(boot_button_timer_, 0);
            }
            if (boot_button_click_count_ == 3) {
                // 三击事件触发
                ESP_LOGI(TAG, "Boot button triple clicked!");
                // TODO: 在这里添加三击要执行的功能
                boot_button_click_count_ = 0;
                xTimerStop(boot_button_timer_, 0);
                ResetWifiConfiguration();
            }
            // 原有单击逻辑
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            // app.ToggleChatState();
        });

        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "Boot button press up");
            
             auto& app = Application::GetInstance();
            app.StopListening();
        });
        boot_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "Boot button press down");
           auto& app = Application::GetInstance();
            app.StartListening();
        });
    }

    // GC9A01-SPI2初始化-用于显示小智
    void InitializeSpiEye1() {
        const spi_bus_config_t buscfg = {       
            .mosi_io_num = GC9A01_SPI1_LCD_GPIO_MOSI,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = GC9A01_SPI1_LCD_GPIO_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = GC9A01_LCD_H_RES * GC9A01_LCD_V_RES * sizeof(uint16_t), // 增大传输大小,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(GC9A01_LCD_SPI1_NUM, &buscfg, SPI_DMA_CH_AUTO));
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
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)GC9A01_LCD_SPI1_NUM, &io_config, &lcd_io1);
        // lcd_io_eye = lcd_io1;
    
        ESP_LOGD(TAG, "Install LCD1 driver");
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GC9A01_SPI1_LCD_GPIO_RST,
            .color_space = GC9A01_LCD_COLOR_SPACE,
            .bits_per_pixel = GC9A01_LCD_BITS_PER_PIXEL,
            
        };
        panel_config.rgb_endian = DISPLAY_RGB_ORDER;
        esp_lcd_new_panel_gc9a01(lcd_io1, &panel_config, &lcd_panel1);
        // lcd_panel_eye = lcd_panel1;
        
        esp_lcd_panel_reset(lcd_panel1);
        esp_lcd_panel_init(lcd_panel1);
        esp_lcd_panel_invert_color(lcd_panel1, true);
        esp_lcd_panel_disp_on_off(lcd_panel1, true);
    }

     // GC9A01-SPI2初始化-用于显示魔眼
     void InitializeSpiEye2() {
        const spi_bus_config_t buscfg = {       
            .mosi_io_num = GC9A01_SPI2_LCD_GPIO_MOSI,
            .miso_io_num = GPIO_NUM_NC,
            .sclk_io_num = GC9A01_SPI2_LCD_GPIO_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = GC9A01_LCD_H_RES * GC9A01_LCD_V_RES * sizeof(uint16_t),
        };
        ESP_ERROR_CHECK(spi_bus_initialize(GC9A01_LCD_SPI2_NUM, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01-SPI2初始化-用于显示魔眼
    void InitializeGc9a01DisplayEye2() {
        ESP_LOGI(TAG, "Init GC9A01 display2");

        ESP_LOGI(TAG, "Install panel IO2");
        ESP_LOGD(TAG, "Install panel IO2");
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = GC9A01_SPI2_LCD_GPIO_CS,
            .dc_gpio_num = GC9A01_SPI2_LCD_GPIO_DC,
            .spi_mode = 0,
            .pclk_hz = GC9A01_LCD_PIXEL_CLK_HZ,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = GC9A01_LCD_CMD_BITS,
            .lcd_param_bits = GC9A01_LCD_PARAM_BITS,
    
    
        };
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)GC9A01_LCD_SPI2_NUM, &io_config, &lcd_io2);
        // lcd_io_eye2 = lcd_io2;
    
        ESP_LOGD(TAG, "Install LCD2 driver");
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GC9A01_SPI2_LCD_GPIO_RST,
            .color_space = GC9A01_LCD_COLOR_SPACE,
            .bits_per_pixel = GC9A01_LCD_BITS_PER_PIXEL
        };
           panel_config.rgb_endian = DISPLAY_RGB_ORDER;
        esp_lcd_new_panel_gc9a01(lcd_io2, &panel_config, &lcd_panel2);
        // lcd_panel_eye2 = lcd_panel2;
        esp_lcd_panel_reset(lcd_panel2);
        esp_lcd_panel_init(lcd_panel2);
        esp_lcd_panel_invert_color(lcd_panel2, true);
        esp_lcd_panel_disp_on_off(lcd_panel2, true);
        
    }

    //初始化双屏
    void InitializeDualScreenEye(){
        // 初始化第一块屏幕
        InitializeSpiEye1();
        InitializeSpiEye2();
        InitializeGc9a01DisplayEye1();
        InitializeGc9a01DisplayEye2();

        // 创建双屏显示对象
        display_ = new EyeToyDisplay(
            lcd_io1, lcd_panel1,
            lcd_io2, lcd_panel2,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            {
                .text_font = &font_puhui_20_4,
                .icon_font = &font_awesome_20_4,
                .emoji_font = font_emoji_64_init(),
            }
        );
        // display_ = new SpiLcdDisplay(lcd_io1, lcd_panel1,
        //     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
        //     {
        //         .text_font = &font_puhui_20_4,
        //         .icon_font = &font_awesome_20_4,
        //         .emoji_font = font_emoji_64_init(),
        //     });
    }


public:

  //没有按键
  XiaoZhiEyeBoard() : boot_button_(BOOT_BUTTON_GPIO) {
    // 按钮模式
    device_mode_ = DeviceMode::BUTTON_MODE;
    gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(AUDIO_CODEC_PA_PIN, GPIO_PULLDOWN_ONLY);
    gpio_set_level(AUDIO_CODEC_PA_PIN, 0); //初始化
    // lcd_mutex_init(); // 初始化互斥锁   
    InitializeI2c();
    InitializeDualScreenEye();
    InitializeButtons();
    initializeIot();
    //GetBacklight()->SetBrightness(100);
}


  virtual AudioCodec* GetAudioCodec() override {
    static Es8311AudioCodec audio_codec(
        codec_i2c_bus_, 
        I2C_NUM_0, 
        AUDIO_INPUT_SAMPLE_RATE, 
        AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, 
        AUDIO_I2S_GPIO_BCLK, 
        AUDIO_I2S_GPIO_WS, 
        AUDIO_I2S_GPIO_DOUT, 
        AUDIO_I2S_GPIO_DIN,
        AUDIO_CODEC_PA_PIN, 
        AUDIO_CODEC_ES8311_ADDR);
    return &audio_codec;
}

    virtual Display* GetDisplay() override {
        return display_;
    }

    //virtual Backlight* GetBacklight() override {
    //    static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    //    return &backlight;
    //}


};

DECLARE_BOARD(XiaoZhiEyeBoard);