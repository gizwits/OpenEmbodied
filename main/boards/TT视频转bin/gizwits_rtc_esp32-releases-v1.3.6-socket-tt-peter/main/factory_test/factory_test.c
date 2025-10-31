#include "driver/uart.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "storage.h"
#include "factory_test.h"
#include "xtask.h"
#include "driver/gpio.h"
#include "test_stream.h"
#include "tt_ledc.h"
#include "uart_ctrl_lcd.h"
#include "ft_raw_music.h"
#include "audio_processor.h"

#ifdef CONFIG_FACTORY_TEST_MODE_ENABLE

#define FACTORY_TEST_UART_NUM UART_NUM_1
#define FACTORY_TEST_UART_TX_PIN    0 // GPIO_NUM_0
#define FACTORY_TEST_UART_RX_PIN    CONFIG_ESP_CONSOLE_UART_RX_GPIO

static const char *TAG = "FT";

#define MAX_AT_CMD_LEN 256
#define AT_BUF_SIZE (MAX_AT_CMD_LEN*2)

// 产测IO通道
static int s_io_pin = -1;
static int s_io_cnt = 0;

// 产测模式
static int s_factory_test_mode = 0;

static void handle_at_command(char *cmd);
static void handle_at_command_buffer(uint8_t *data);

static void factory_test_task(void *arg)
{
    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(AT_BUF_SIZE+1);

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(FACTORY_TEST_UART_NUM, data, (AT_BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        if (len) {
            data[len] = '\0';
            ESP_LOGI(TAG, "RX: %s", (char *) data);
            // 处理AT命令
            handle_at_command_buffer(data);
        }
    }
}

// 初始化产测串口
void factory_test_uart_init(void) {
    // 配置串口参数
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 安装UART驱动
    ESP_ERROR_CHECK(uart_driver_install(FACTORY_TEST_UART_NUM, AT_BUF_SIZE, MAX_AT_CMD_LEN, 8, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FACTORY_TEST_UART_NUM, &uart_config));
    
    // 设置UART引脚
    // ESP_ERROR_CHECK(uart_set_pin(CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_TX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_pin(FACTORY_TEST_UART_NUM, FACTORY_TEST_UART_TX_PIN, FACTORY_TEST_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreateExt(factory_test_task, "factory_test", 4 * 1024, NULL, 5, NULL);

    ESP_LOGW(TAG, "Factory Test initialized successfully");
}

// 发送数据到串口
static void factory_test_send(const char *data, int len) {
    if (data == NULL) {
        ESP_LOGE(TAG, "Invalid data to send");
        return;
    }

    ESP_LOGI(TAG, "TX[%d]: %s", len, data);
    if (len > 0) {
        uart_write_bytes(FACTORY_TEST_UART_NUM, data, len);
    }
}

// 初始化产测
void factory_test_init(void) {
    // 加载配置
    storage_load_factory_test_mode(&s_factory_test_mode);

    // 初始化产测串口
    factory_test_uart_init();

    // 初始化IO通道
    s_io_pin = -1;
    s_io_cnt = 0;

    // 延时等待
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_factory_test_mode == FACTORY_TEST_MODE_IN_FACTORY) {
        // 发送产测命令
        factory_test_send("+ENTER_TEST OK\r\n", strlen("+ENTER_TEST OK\r\n"));
    }

}

// 产测是否开启
bool factory_test_is_enabled(void) {
    return (s_factory_test_mode == FACTORY_TEST_MODE_IN_FACTORY);
}

bool factory_test_is_aging(void) {
    return (s_factory_test_mode == FACTORY_TEST_MODE_AGING || s_factory_test_mode == FACTORY_TEST_MODE_AGING_FINISHED);
}

// IO测试任务
static void io_test_task(void *arg) {
    while (s_factory_test_mode) {
        // 检查IO引脚是否有效
        if (s_io_pin < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 读取IO引脚状态并进行脉冲检测
        static int last_level = 0;
        int current_level = gpio_get_level(s_io_pin);
        
        // 检测上升沿
        if (last_level == 0 && current_level == 1) {
            s_io_cnt++;
            ESP_LOGI(TAG, "Pulse detected on IO pin %d, count: %d", s_io_pin, s_io_cnt);
        }
        last_level = current_level;

        // 延时等待
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

// 初始化IO测试
static esp_err_t init_io_test(const char *cmd) {
    // 解析命令参数
    int channel, io_pin, pulse_period, pulse_count;
    if (sscanf(cmd, "AT+IOTEST=%d,%d,%d,%d", &channel, &io_pin, &pulse_period, &pulse_count) != 4) {
        ESP_LOGE(TAG, "Invalid IO test command format");
        return ESP_FAIL;
    }

    // 验证参数有效性
    if (io_pin < 0 || io_pin > 40) {
        ESP_LOGE(TAG, "Invalid IO pin: %d", io_pin);
        return ESP_FAIL;
    }

    // 配置IO引脚为输入模式
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << io_pin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // 保存测试参数
    s_io_pin = io_pin;
    s_io_cnt = 0;
    
    // 创建IO测试任务
    if (xTaskGetHandle("io_test") == NULL) {
        xTaskCreateExt(io_test_task, "io_test", 1024*4, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "IO test initialized: pin=%d", io_pin);
    return ESP_OK;
}

// 定义全局缓冲区
static char at_buffer[MAX_AT_CMD_LEN+10] = {0};
static int at_buffer_len = 0;

static void save_factory_test_mode_task(void *arg) {
    int mode = (int)arg;
    
    // 保存产测模式
    s_factory_test_mode = mode;
    esp_err_t ret = storage_save_factory_test_mode(mode);

    if (mode == FACTORY_TEST_MODE_NONE) {
        // 退出产测
        storage_clear_wifi_config();
        user_set_volume(80);
        if (ret == ESP_OK) {
            factory_test_send("+EXIT_TEST OK\r\n", strlen("+EXIT_TEST OK\r\n"));
            vTaskDelay(500 / portTICK_PERIOD_MS);
            factory_test_send("+EXIT_TEST OK\r\n", strlen("+EXIT_TEST OK\r\n"));
            vTaskDelay(500 / portTICK_PERIOD_MS);
        } else {
            factory_test_send("+EXIT_TEST ERROR\r\n", strlen("+EXIT_TEST ERROR\r\n"));
            vTaskDelay(500 / portTICK_PERIOD_MS);
            factory_test_send("+EXIT_TEST ERROR\r\n", strlen("+EXIT_TEST ERROR\r\n"));
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    } else if (mode == FACTORY_TEST_MODE_IN_FACTORY) {
        // 进入产测时，连接产测路由器
        storage_save_wifi_config(FACTORY_TEST_SSID, FACTORY_TEST_PASSWORD);
        ESP_LOGW(TAG, "=== Save factory test SSID: %s", FACTORY_TEST_SSID);
        user_set_volume(80);
        ESP_LOGW(TAG, "=== Set factory test volume to 80");
    }

    // 重启
    ESP_LOGI(TAG, "Restarting...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    esp_restart();
}

// 处理AT命令
static void handle_at_command(char *cmd) {
    esp_err_t ret = ESP_FAIL;

    if (strcmp(cmd, "AT+ENTER_TEST") == 0) {
        // 处理进入产测命令
        ESP_LOGI(TAG, "Received enter factory test command");
        xTaskCreate(save_factory_test_mode_task, "save_factory_test_mode", 1024*4,
                    (void *)FACTORY_TEST_MODE_IN_FACTORY, 5, NULL);
    }
    else if (strcmp(cmd, "AT+EXIT_TEST") == 0) {
        // 处理退出产测命令
        ESP_LOGI(TAG, "Received exit factory test command");
        xTaskCreate(save_factory_test_mode_task, "save_factory_test_mode", 1024*4,
                    (void *)FACTORY_TEST_MODE_NONE, 5, NULL);
    }
    else if (strcmp(cmd, "AT+VER") == 0) {
        char version_str[64];

        // 处理版本查询命令
        ESP_LOGI(TAG, "Received version query command");
        // 返回版本信息
        snprintf(version_str, sizeof(version_str), "+VER %s,%s,%s\r\n", HARD_VERSION, SOFT_VERSION, 
                 storage_is_auth_valid() ? "auth ok" : "auth failed");
        factory_test_send(version_str, strlen(version_str));
    }
    else if (strncmp(cmd, "AT+IOTEST=", strlen("AT+IOTEST=")) == 0) {
        // 处理IO测试命令
        // AT+IOTEST=产测底板通道N，待测试底板IO_PIN，脉冲周期ms，脉冲次数
        ESP_LOGI(TAG, "Received IO test start command");
        ret = init_io_test(cmd);
        // 返回OK响应
        if (ret == ESP_OK) {
            factory_test_send("OK\r\n", strlen("OK\r\n"));
        } else {
            factory_test_send("ERROR\r\n", strlen("ERROR\r\n"));
        }
    }
    else if (strcmp(cmd, "AT+IOTEST?") == 0) {
        char result_str[64];
        // 查询IO测试结果
        ESP_LOGI(TAG, "Received IO test query command");
        snprintf(result_str, sizeof(result_str), "+IOTEST=%d\r\n", s_io_cnt);
        factory_test_send(result_str, strlen(result_str));
    }
    else if (strncmp(cmd, "AT+REC=1", strlen("AT+REC=1")) == 0) {
        // 处理录音命令
        ESP_LOGI(TAG, "Received record command");
        // 返回录音成功响应
        if (ft_start_play_task(3, ft_raw_music, ft_raw_music_size) == 0
            && ft_start_record_task(CHANNEL_AEC, 4) == 0) {
            ESP_LOGI(TAG, "Play & Record task started");
            factory_test_send("+REC OK\r\n", strlen("+REC OK\r\n"));
        } else {
            ESP_LOGE(TAG, "Record task failed to start");
            factory_test_send("+REC ERROR\r\n", strlen("+REC ERROR\r\n"));
        }
    }
    else if (strncmp(cmd, "AT+REC=0", strlen("AT+REC=0")) == 0) {
        // 处理录音命令
        ESP_LOGI(TAG, "Received record command");
        // 返回录音成功响应
        if (ft_start_record_task(CHANNEL_MIC, 4) == 0) {
            ESP_LOGI(TAG, "Record task started");
            factory_test_send("+REC OK\r\n", strlen("+REC OK\r\n"));
        } else {
            ESP_LOGE(TAG, "Record task failed to start");
            factory_test_send("+REC ERROR\r\n", strlen("+REC ERROR\r\n"));
        }
    }
    else if (strncmp(cmd, "AT+PLAY=0", strlen("AT+PLAY=0")) == 0) {
        // 处理播放命令
        ESP_LOGI(TAG, "Received play command");
        // 返回播放成功响应
        if (ft_start_play_task(8, ft_get_record_buffer(), ft_get_record_buffer_index()) == 0) {
            ESP_LOGI(TAG, "Play task started");
            factory_test_send("+PLAY OK\r\n", strlen("+PLAY OK\r\n"));
        } else {
            ESP_LOGE(TAG, "Play task failed to start");
            factory_test_send("+PLAY ERROR\r\n", strlen("+PLAY ERROR\r\n"));
        }
    }
    else if (strncmp(cmd, "AT+RSSI?", strlen("AT+RSSI?")) == 0) {
        ESP_LOGI(TAG, "Received get RSSI command");
        int32_t rssi = 0;
        wifi_ap_record_t ap_info;
        if (get_wifi_is_connected() == 1) {
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi; // 获取信号强度
                ESP_LOGE(TAG,"WiFi Signal RSSI: %d dBm", rssi);
            }
        }
        char rssi_str[64];

        // 返回RSSI信息
        snprintf(rssi_str, sizeof(rssi_str), "+RSSI=%d\r\n", rssi);
        factory_test_send(rssi_str, strlen(rssi_str));
    }
    else {
        // 未知命令处理
        ESP_LOGW(TAG, "Unknown AT command[%d]: %s", strlen(cmd), cmd);
        factory_test_send("+ERROR\r\n", strlen("+ERROR\r\n"));
    }
}
// 处理接收到的数据
static void handle_at_command_buffer(uint8_t *data) {
    static int64_t last_receive_time = 0;  // 上次接收时间
    int64_t now = esp_timer_get_time() / 1000;  // 当前时间，单位ms
    
    // 如果距离上次接收超过1s，清空缓冲区
    if (now - last_receive_time > 1000) {
        ESP_LOGW(TAG, "Receive interval exceeds 1s, clearing buffer");
        at_buffer_len = 0;
        memset(at_buffer, 0, sizeof(at_buffer));
    }
    last_receive_time = now;

    int data_len = strlen((char *)data);
    int processed_len = 0;

    while (processed_len < data_len) {
        // 计算本次可处理的长度
        int available_space = sizeof(at_buffer) - at_buffer_len - 1;
        int copy_len = (data_len - processed_len) < available_space ? 
                      (data_len - processed_len) : available_space;

        // 将数据追加到缓冲区
        memcpy(at_buffer + at_buffer_len, data + processed_len, copy_len);
        at_buffer_len += copy_len;
        processed_len += copy_len;

        // 处理缓冲区中的完整命令
        while (1) {
            char *line_end = strstr(at_buffer, "\r\n");
            if (line_end == NULL) {
                // 没有完整行，检查是否需要清空缓冲区
                if (at_buffer_len >= sizeof(at_buffer) - 1) {
                    ESP_LOGW(TAG, "AT buffer too long, clearing buffer");
                    at_buffer_len = 0;
                    memset(at_buffer, 0, sizeof(at_buffer));
                }
                break;
            }

            // 处理完整行
            *line_end = '\0';
            ESP_LOGI(TAG, "Processing AT command[%d]: %s", strlen(at_buffer), at_buffer);
            // AT指令处理逻辑
            handle_at_command(at_buffer);

            // 移除已处理的数据
            int remaining_len = at_buffer_len - (line_end - at_buffer + 2);
            memmove(at_buffer, line_end + 2, remaining_len);
            at_buffer_len = remaining_len;
            memset(at_buffer + at_buffer_len, 0, sizeof(at_buffer) - at_buffer_len);
        }
    }
}

void key_on_rec_break_pressed(void)
{
    static int64_t last_event_time = 0;  // 单位：ms
    static int event_count = 0;
    
    // 获取当前时间
    int64_t now = esp_timer_get_time() / 1000;  // 转换为ms
    
    // 如果距离上次事件超过2000ms，则重置计数
    if (now - last_event_time > 2000) {
        event_count = 0;
    }
    
    // 更新事件时间和计数
    last_event_time = now;
    event_count++;
    
    ESP_LOGI(TAG, "Record break event count: %d", event_count);
    if (event_count == 10) {

        if (!factory_test_is_aging()) {
            ESP_LOGI(TAG, "Enter aging test mode");
            s_factory_test_mode = FACTORY_TEST_MODE_AGING;
            storage_save_factory_test_mode(FACTORY_TEST_MODE_AGING);
            audio_tone_play(1, 1, "spiffs://spiffs/enter_aging.mp3");
        } else {
            char ssid[32];
            char password[64];
            
            ESP_LOGI(TAG, "Exit aging test mode");
            s_factory_test_mode = FACTORY_TEST_MODE_NONE;
            storage_save_factory_test_mode(FACTORY_TEST_MODE_NONE);
            audio_tone_play(1, 1, "spiffs://spiffs/exit_aging.mp3");

            bool success = storage_load_wifi_config(ssid, password);

            if (!success) {
                // 如果设备没有配置过wifi，则配置产测模式下的wifi
                storage_save_wifi_config(FACTORY_TEST_SSID, FACTORY_TEST_PASSWORD);
                ESP_LOGW(TAG, "=== Save factory test default wifi config");
            }
        }

        // 延时重启
        ESP_LOGI(TAG, "Restarting...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }
}


uint16_t set_factory_auto_color(int32_t delay_ms)
{
    // 增加产测逻辑
    static uint8_t color_index = 0;
    static TickType_t last_tick = 0;
    TickType_t current_tick = xTaskGetTickCount();
    if(last_tick == 0) {
        last_tick = current_tick;
    }
    // while (current_video == VIDEO_FACTORY_TEST) {
        current_tick = xTaskGetTickCount();
        // INSERT_YOUR_CODE
    if ((current_tick - last_tick) * portTICK_PERIOD_MS >= delay_ms) { // 每秒切换颜色
        last_tick = current_tick;

        switch (color_index) {
            case 0:
                tt_led_strip_set_state(TT_LED_STATE_RED);
                lcd_state_event_send(EVENT_RED);
                break;
            case 1:
                tt_led_strip_set_state(TT_LED_STATE_GREEN);
                lcd_state_event_send(EVENT_GREEN);
                break;
            case 2:
                tt_led_strip_set_state(TT_LED_STATE_BLUE);
                lcd_state_event_send(EVENT_BLUE);
                break;
            case 3:
                tt_led_strip_set_state(TT_LED_STATE_YELLOW);
                lcd_state_event_send(EVENT_YELLOW);
                break;
        }
        ESP_LOGI("LCD", "color_index", color_index);
        
        color_index = (color_index + 1) % 4; // 循环切换颜色
    }
}

static void play_aging_tone(void) { 
    static TickType_t last_play_tick = 0; // 将静态变量移出循环
    TickType_t current_play_tick = xTaskGetTickCount();
    if ((current_play_tick - last_play_tick) * portTICK_PERIOD_MS >= 10000) { // 每10秒播放
        last_play_tick = current_play_tick;
        ESP_LOGI(TAG, "Playing aging test tone");
        audio_tone_play(0, 0, "spiffs://spiffs/bo.mp3");
    }
}


static void factory_test_lcd_task(void *pvParameters) {
    
    // 定义播放间隔时间
    const TickType_t xDelay = 100 / portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "Factory test lcd task started");

    while (1) {
        // 获取当前时间
        int64_t now = esp_timer_get_time() / 1000;  // 转换为ms
        set_factory_auto_color(1000);
        // 等待指定时间
        vTaskDelay(xDelay);
    }

    ESP_LOGI(TAG, "Factory test lcd task ended");
    vTaskDelete(NULL);
}

// 启动老化测试任务
void factory_start_lcd_task(void) {
    // 创建老化测试任务
    xTaskCreate(factory_test_lcd_task, "factory_test_lcd_task", 1024*4, NULL, 5, NULL);
    ESP_LOGI(TAG, "LCD test task started");
}

static void factory_test_aging_task(void *pvParameters) {
    // 设置初始音量
    user_set_volume(80);

    
    // 定义播放间隔时间
    const TickType_t xDelay = 5 * 1000 / portTICK_PERIOD_MS;

    // 老化开始时间
    int64_t aging_start_time = esp_timer_get_time() / 1000; // 转换为ms

    while (1) {
        // 播放提示音
        ESP_LOGW(TAG, "Aging test mode: %s", s_factory_test_mode == FACTORY_TEST_MODE_AGING ? "UNDER_AGING" : "AGING_FINISHED");
        // 获取当前时间
        int64_t now = esp_timer_get_time() / 1000;  // 转换为ms
        // 计算老化时间（毫秒）
        int64_t aging_time = now - aging_start_time;
        if (s_factory_test_mode == FACTORY_TEST_MODE_AGING_FINISHED) {
            audio_tone_play(1, 1, "spiffs://spiffs/aging_finished.mp3");
        } else if (s_factory_test_mode == FACTORY_TEST_MODE_AGING) {
            if (aging_time < (30 * 60 * 1000)) { // 小于30分钟
                audio_tone_play(1, 1, "spiffs://spiffs/under_aging.mp3");
            } else {
                ESP_LOGI(TAG, "=== Aging test finished");
                s_factory_test_mode = FACTORY_TEST_MODE_AGING_FINISHED;
                storage_save_factory_test_mode(FACTORY_TEST_MODE_AGING_FINISHED);
                audio_tone_play(1, 1, "spiffs://spiffs/aging_finished.mp3");
            }
        }
        set_factory_auto_color(1000);
        // 等待指定时间
        vTaskDelay(xDelay);
    }
}

// 启动老化测试任务
void factory_start_aging_task(void) {
    // 创建老化测试任务
    xTaskCreate(factory_test_aging_task, "factory_test_aging_task", 1024*4, NULL, 5, NULL);
    ESP_LOGI(TAG, "Aging test task started");
}


void handle_aging_test_mode() {
    if (!factory_test_is_aging()) {
        ESP_LOGI(TAG, "Enter aging test mode");
        s_factory_test_mode = FACTORY_TEST_MODE_AGING;
        storage_save_factory_test_mode(FACTORY_TEST_MODE_AGING);
        audio_tone_play(1, 1, "spiffs://spiffs/enter_aging.mp3");
    } else {
        char ssid[32];
        char password[64];
        
        ESP_LOGI(TAG, "Exit aging test mode");
        s_factory_test_mode = FACTORY_TEST_MODE_NONE;
        storage_save_factory_test_mode(FACTORY_TEST_MODE_NONE);
        audio_tone_play(1, 1, "spiffs://spiffs/exit_aging.mp3");

        // 如果设备没有配置过wifi，则配置产测模式下的wifi
        storage_save_wifi_config(FACTORY_TEST_SSID, FACTORY_TEST_PASSWORD);
        ESP_LOGW(TAG, "=== Save factory test default wifi config");
    }

    // 延时重启
    ESP_LOGI(TAG, "Restarting...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
}

#endif
