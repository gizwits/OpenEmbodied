#include "driver/uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "factory_test.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "auth.h"
#include "esp_ota_ops.h"
#include "system_info.h"
#include "esp_app_format.h"
#include <cstring>
#include <cstdio>
#include "assets/lang_config.h"
#include "application.h"
#include "audio/audio_codec.h"
#include "board.h"
#include <string>
#include <wifi_station.h>
#include "config.h"

static const char *TAG = "factory_test";

// 工厂测试音频功能包装函数
static int ft_start_record_task(int duration_seconds) {
    return Application::GetInstance().StartRecordTest(duration_seconds);
}

static int ft_start_play_task(int duration_seconds) {
    ESP_LOGI(TAG, "StartPlayTest: duration=%d seconds", duration_seconds);
    return Application::GetInstance().StartPlayTest(duration_seconds);
}


// 缺失函数的简单实现
static esp_err_t storage_save_factory_test_mode(int mode) {
    Settings settings("wifi", true);
    settings.SetInt("ft_mode", mode);  // 使用更短的键名
    return ESP_OK;
}

static bool storage_is_auth_valid() {
    // 通过Auth类的getAuthKey()判断是否授权
    bool has_authkey = !Auth::getInstance().getAuthKey().empty();
    return has_authkey;
}

static void user_set_volume(int volume) {
    ESP_LOGI(TAG, "Set volume: %d", volume);
    // 这里可以添加实际的音量设置实现
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    codec->SetOutputVolume(static_cast<uint8_t>(volume));
}

// 获取版本信息
static std::string get_hardware_version() {
    ESP_LOGI(TAG, "Get hardware version: %s", BOARD_NAME);
    return BOARD_NAME;
}

static std::string get_software_version() {
    ESP_LOGI(TAG, "Get software version: %s", esp_app_get_description()->version);
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Get software version: %s", app_desc->version);
    return std::string(app_desc->version);
}

#ifdef CONFIG_FACTORY_TEST_MODE_ENABLE

#define MAX_AT_CMD_LEN 256
#define AT_BUF_SIZE (MAX_AT_CMD_LEN*2)

// 产测IO通道
static int s_io_pin = -1;
static int s_io_cnt = 0;

// 产测模式
static int s_factory_test_mode = 0;

// 标记是否已经接管了串口
static bool s_uart_taken_over = false;

static void handle_at_command(char *cmd);
static void handle_at_command_buffer(uint8_t *data);

static void factory_test_task(void *arg)
{
#ifdef FACTORY_TEST_UART_RX_PIN
    // Configure a temporary buffer for the incoming data
    uint8_t *data = static_cast<uint8_t*>(malloc(AT_BUF_SIZE+1));
    
    ESP_LOGI(TAG, "Factory test task started, waiting for data...");

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(FACTORY_TEST_UART_NUM, data, (AT_BUF_SIZE - 1), 20 / portTICK_PERIOD_MS);
        
        // 添加调试信息
        static int debug_counter = 0;
        debug_counter++;
        if (debug_counter % 100 == 0) {  // 每100次循环打印一次
            // ESP_LOGI(TAG, "UART read loop, counter: %d, last read len: %d", debug_counter, len);
            printf("#");
        }
        
        if (len > 0) {
            data[len] = '\0';
            ESP_LOGI(TAG, "RX[%d]: %s", len, reinterpret_cast<const char*>(data));
            hexdump("FT RX",data, len);

            // 处理AT命令
            handle_at_command_buffer(data);
            ESP_LOGI(TAG, "RX[%d] done", len);
        } else if (len < 0) {
            ESP_LOGE(TAG, "UART read error: %d", len);
        }
        
        // 短暂延时，避免CPU占用过高
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
}

// 初始化产测串口
void factory_test_uart_init(void) {
#ifdef FACTORY_TEST_UART_RX_PIN
    // 如果已经接管了串口，直接返回
    if (s_uart_taken_over) {
        ESP_LOGI(TAG, "UART already taken over, skipping initialization");
        return;
    }

    ESP_LOGI(TAG, "Initializing factory test UART on UART_NUM_0");
    ESP_LOGI(TAG, "TX Pin: %d, RX Pin: %d", FACTORY_TEST_UART_TX_PIN, FACTORY_TEST_UART_RX_PIN);

    // 检查UART是否已经被其他组件使用
    esp_err_t ret = uart_driver_install(FACTORY_TEST_UART_NUM, AT_BUF_SIZE, MAX_AT_CMD_LEN, 8, nullptr, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART_NUM_0 is already in use, trying to uninstall first");
        uart_driver_delete(FACTORY_TEST_UART_NUM);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = uart_driver_install(FACTORY_TEST_UART_NUM, AT_BUF_SIZE, MAX_AT_CMD_LEN, 8, nullptr, 0);
    }
    ESP_ERROR_CHECK(ret);

    // 配置串口参数
    uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,  // 使用标准波特率
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(FACTORY_TEST_UART_NUM, &uart_config));
    
    // 设置UART引脚
    ESP_ERROR_CHECK(uart_set_pin(FACTORY_TEST_UART_NUM, FACTORY_TEST_UART_TX_PIN, FACTORY_TEST_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // 标记已接管串口
    s_uart_taken_over = true;

    xTaskCreate(factory_test_task, "factory_test", 4 * 1024, nullptr, 8, nullptr);

    ESP_LOGW(TAG, "Factory Test UART initialized successfully");
#endif
}

// 发送数据到串口 - 通过错误日志发送
static void factory_test_send(const char *data, int len) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "Invalid data to send");
        return;
    }

    // ESP_LOGI(TAG, "TX[%d]: %s", len, data);
    if (len > 0) {
        // 如果以+ENTER_TEST开头，直接返回OK
        if (strncmp(data, "+ENTER_TEST", strlen("+ENTER_TEST")) == 0
            || strncmp(data, "+EXIT_TEST", strlen("+EXIT_TEST")) == 0
            || strncmp(data, "+REC", strlen("+REC")) == 0) {
            // 通过print发送数据，这样可以在串口上看到
            // 发送三次确保对方能收到
            for (int i = 0; i < 3; i++) {
                printf("\r\n%.*s\r\n", len, data);
                // 短暂延迟，避免数据重叠
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            if (strncmp(data, "+EXIT_TEST", strlen("+EXIT_TEST")) == 0) {
                ESP_LOGI(TAG, "Exit factory test, delay 2 seconds");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        } else {
            printf("\r\n%.*s\r\n", len, data);
            // 短暂延迟，避免数据重叠
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// 初始化产测
void factory_test_init(void) {
    Settings settings("wifi", true);
    s_factory_test_mode = settings.GetInt("ft_mode", 0);  // 使用更短的键名
    ESP_LOGI(TAG, "产测模式: %d", s_factory_test_mode);
    if (s_factory_test_mode == FACTORY_TEST_MODE_IN_FACTORY) {
        // 产测模式临时连接产测路由器
        auto& wifi_station = WifiStation::GetInstance();
        wifi_station.OnScanBegin([]() {
            ESP_LOGI(TAG, "Scanning WiFi...");
        });
        wifi_station.OnConnect([](const std::string& ssid) {
            ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid.c_str());
        });
        wifi_station.OnConnected([](const std::string& ssid) {
            ESP_LOGI(TAG, "Connected to WiFi: %s", ssid.c_str());
        });

        // 插入 ssid
        wifi_station.AddAuth(FACTORY_TEST_SSID, FACTORY_TEST_PASSWORD);

        wifi_station.Start();
        
        // ESP_LOGI(TAG, "产测模式临时连接产测路由器");
        // if (!wifi_station.WaitForConnected(30 * 1000)) {
        //     // wifi_station.Stop();
        // }
    }
}

void factory_test_start(void) {
    ESP_LOGI(TAG, "Starting factory test, mode: %d", s_factory_test_mode);
    
    // 初始化产测串口
    factory_test_uart_init();

    // 初始化IO通道
    s_io_pin = -1;
    s_io_cnt = 0;

    // 延时等待
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_factory_test_mode == FACTORY_TEST_MODE_IN_FACTORY) {
        // 发送产测命令
        ESP_LOGI(TAG, "Sending factory test entry confirmation");
        factory_test_send("+ENTER_TEST OK", strlen("+ENTER_TEST OK"));
    } else if (s_factory_test_mode == FACTORY_TEST_MODE_NONE) {
        // 发送产测命令
        ESP_LOGI(TAG, "Sending factory test exit confirmation");
        factory_test_send("+EXIT_TEST OK", strlen("+EXIT_TEST OK"));
    }
}

// 产测是否开启
bool factory_test_is_enabled(void) {
    return (s_factory_test_mode == FACTORY_TEST_MODE_IN_FACTORY);
}

bool factory_test_is_aging(void) {
    return (s_factory_test_mode == FACTORY_TEST_MODE_AGING);
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
        int current_level = gpio_get_level(static_cast<gpio_num_t>(s_io_pin));
        
        // 检测上升沿
        if (last_level == 0 && current_level == 1) {
            s_io_cnt++;
            ESP_LOGI(TAG, "Pulse detected on IO pin %d, count: %d", s_io_pin, s_io_cnt);
        }
        last_level = current_level;

        // 延时等待
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    if (xTaskGetHandle("io_test") == nullptr) {
        xTaskCreate(io_test_task, "io_test", 1024*4, nullptr, 5, nullptr);
    }

    ESP_LOGI(TAG, "IO test initialized: pin=%d", io_pin);
    return ESP_OK;
}

// 定义全局缓冲区
static char at_buffer[MAX_AT_CMD_LEN+10] = {0};
static int at_buffer_len = 0;

void save_factory_test_mode_task(void *arg) {
    int mode = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    // esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "free memory: %d", (int)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // 保存产测模式
    s_factory_test_mode = mode;
    esp_err_t ret = storage_save_factory_test_mode(mode);
    // if (mode != 0) {
    //     ESP_LOGI(TAG, "Save factory test mode: %d", mode);
    //     ret = storage_save_factory_test_mode(mode);
    //     ESP_LOGI(TAG, "Save factory test mode: %d, ret: %d", mode, ret);
    // }

    if (mode == 0) {
        // 退出产测
        if (ret == ESP_OK) {
#ifdef CONFIG_TMP_PRODUCT_TEST_WIFI
            // 进入临时产测模式
            Settings settings("wifi", true);
            settings.SetInt("tmp_ft_mode", 1);
#endif
            factory_test_send("+EXIT_TEST OK", strlen("+EXIT_TEST OK"));
        } else {
            factory_test_send("+EXIT_TEST ERROR", strlen("+EXIT_TEST ERROR"));
        }
    } else {
        // 进入产测
        if (ret == ESP_OK) {
            factory_test_send("+ENTER_TEST OK", strlen("+ENTER_TEST OK"));
        } else {
            factory_test_send("+ENTER_TEST ERROR", strlen("+ENTER_TEST ERROR"));
        }
    }
    
    // 延时后重启
    ESP_LOGI(TAG, "Restarting in 2 seconds...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    esp_restart();
}

// 处理AT命令
static void handle_at_command(char *cmd) {
    esp_err_t ret = ESP_FAIL;

    if (strcmp(cmd, "AT+ENTER_TEST") == 0) {
        // 处理进入产测命令
        ESP_LOGI(TAG, "Received enter factory test command");
        auto& wifi_station = WifiStation::GetInstance();
        wifi_station.ClearAuth();
        xTaskCreate(save_factory_test_mode_task, "save_factory_test_mode", 1024*4,
                    reinterpret_cast<void*>(static_cast<intptr_t>(FACTORY_TEST_MODE_IN_FACTORY)), 5, nullptr);
    }
    else if (strcmp(cmd, "AT+EXIT_TEST") == 0) {
        // 处理退出产测命令
        ESP_LOGI(TAG, "Received exit factory test command");
        xTaskCreate(save_factory_test_mode_task, "save_factory_test_mode", 1024*4,
                    reinterpret_cast<void*>(static_cast<intptr_t>(FACTORY_TEST_MODE_NONE)), 5, nullptr);
    }
    else if (strcmp(cmd, "AT+VER") == 0) {
        char version_str[64];

        // 处理版本查询命令
        ESP_LOGI(TAG, "Received version query command");
        // 返回版本信息
        bool auth_valid = storage_is_auth_valid();
        std::string hw_version = get_hardware_version();
        std::string sw_version = get_software_version();
        
        snprintf(version_str, sizeof(version_str), "+VER %s,%s,%s", 
                 hw_version.c_str(), sw_version.c_str(),
                 auth_valid ? "auth ok" : "auth failed");
        ESP_LOGI(TAG, "Version query - HW: %s, SW: %s, auth: %s", 
                 hw_version.c_str(), sw_version.c_str(), 
                 auth_valid ? "ok" : "failed");
        factory_test_send(version_str, strlen(version_str));
    }
    else if (strncmp(cmd, "AT+IOTEST=", strlen("AT+IOTEST=")) == 0) {
        // 处理IO测试命令
        // AT+IOTEST=产测底板通道N，待测试底板IO_PIN，脉冲周期ms，脉冲次数
        ESP_LOGI(TAG, "Received IO test start command");
        ret = init_io_test(cmd);
        // 返回OK响应
        if (ret == ESP_OK) {
            factory_test_send("OK", strlen("OK"));
        } else {
            factory_test_send("ERROR", strlen("ERROR"));
        }
    }
    else if (strcmp(cmd, "AT+IOTEST?") == 0) {
        char result_str[64];
        // 查询IO测试结果
        ESP_LOGI(TAG, "Received IO test query command");
        snprintf(result_str, sizeof(result_str), "+IOTEST=%d", s_io_cnt);
        factory_test_send(result_str, strlen(result_str));
    }
    else if (strncmp(cmd, "AT+REC=0", strlen("AT+REC=0")) == 0) {
        // 处理录音命令
        ESP_LOGI(TAG, "Received record command");
        xTaskCreate([](void* arg) {
            // 返回录音成功响应
            if (ft_start_record_task(2) == 0) {
                ESP_LOGI(TAG, "Record task started");
                factory_test_send("+REC OK", strlen("+REC OK"));
            } else {
                ESP_LOGE(TAG, "Record task failed to start");
                factory_test_send("+REC ERROR", strlen("+REC ERROR"));
            }
            vTaskDelete(NULL);
        }, "audio_record", 1024 * 5, nullptr, 8, nullptr);
    }
    else if (strncmp(cmd, "AT+PLAY=0", strlen("AT+PLAY=0")) == 0) {
        // 处理播放命令
        
        ESP_LOGI(TAG, "Received play command 1");
        xTaskCreate([](void* arg) {
            if (ft_start_play_task(2) == 0) {
                factory_test_send("+PLAY OK", strlen("+PLAY OK"));
            } else {
                ESP_LOGE(TAG, "Play task failed to start");
                factory_test_send("+PLAY ERROR", strlen("+PLAY ERROR"));
            }
            vTaskDelete(NULL);
        }, "audio_play", 1024 * 4, nullptr, 8, nullptr);
    }else if (strncmp(cmd, "AT+RSSI?", strlen("AT+RSSI?")) == 0) {
        ESP_LOGI(TAG, "Received get RSSI command");
        int32_t rssi = 0;
        wifi_ap_record_t ap_info;
        if (WifiStation::GetInstance().IsConnected()) {
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi; // 获取信号强度
                ESP_LOGE(TAG,"WiFi Signal RSSI: %ld dBm", rssi);
            }
        }
        char rssi_str[64];

        // 返回RSSI信息
        snprintf(rssi_str, sizeof(rssi_str), "+RSSI=%ld", rssi);
        factory_test_send(rssi_str, strlen(rssi_str));
    }
    else {
        // 未知命令处理
        ESP_LOGW(TAG, "Unknown AT command[%d]: %s", strlen(cmd), cmd);
        factory_test_send("+ERROR", strlen("+ERROR"));
    }
}
// 处理接收到的数据
static void handle_at_command_buffer(uint8_t *data) {
    if (data == nullptr) {
        ESP_LOGE(TAG, "Invalid input data");
        return;
    }

    static int64_t last_receive_time = 0;
    int64_t now = esp_timer_get_time() / 1000;

    // 如果距离上次接收超过1s，清空缓冲区
    if (now - last_receive_time > 1000) {
        ESP_LOGW(TAG, "Receive interval exceeds 1s, clearing buffer");
        at_buffer_len = 0;
        memset(at_buffer, 0, sizeof(at_buffer));
    }
    last_receive_time = now;

    // 安全计算数据长度
    size_t data_len = strnlen(reinterpret_cast<char*>(data), AT_BUF_SIZE);
    size_t processed_len = 0;

    while (processed_len < data_len) {
        // 确保缓冲区有足够空间
        if (at_buffer_len >= sizeof(at_buffer) - 1) {
            ESP_LOGW(TAG, "Buffer full, clearing");
            at_buffer_len = 0;
            memset(at_buffer, 0, sizeof(at_buffer));
            break;
        }

        // 安全计算可用空间和复制长度
        size_t available_space = sizeof(at_buffer) - at_buffer_len - 1;
        size_t remaining_data = data_len - processed_len;
        size_t copy_len = (remaining_data < available_space) ? remaining_data : available_space;

        // 复制数据到缓冲区
        if (copy_len > 0) {
            memcpy(at_buffer + at_buffer_len, data + processed_len, copy_len);
            at_buffer_len += copy_len;
            processed_len += copy_len;
            at_buffer[at_buffer_len] = '\0';  // 确保字符串结束
        }

        // 处理完整命令
        while (at_buffer_len > 0) {
            char *line_end = strstr(at_buffer, "\r\n");
            if (!line_end) {
                break;
            }

            // 计算命令长度并验证
            ptrdiff_t cmd_len = line_end - at_buffer;
            if (cmd_len < 0 || cmd_len >= static_cast<ptrdiff_t>(sizeof(at_buffer))) {
                ESP_LOGE(TAG, "Invalid command length");
                at_buffer_len = 0;
                memset(at_buffer, 0, sizeof(at_buffer));
                break;
            }

            // 暂存命令并处理
            *line_end = '\0';
            ESP_LOGI(TAG, "Processing AT command[%d]: %s", (int)cmd_len, at_buffer);
            
            // 处理命令前记录状态
            size_t original_buffer_len = at_buffer_len;
            handle_at_command(at_buffer);
            ESP_LOGI(TAG, "Command processing complete");

            // 安全移除已处理的命令
            size_t cmd_total_len = cmd_len + 2;  // 包含 \r\n
            if (cmd_total_len <= original_buffer_len) {
                size_t remaining_len = original_buffer_len - cmd_total_len;
                if (remaining_len > 0) {
                    memmove(at_buffer, line_end + 2, remaining_len);
                    at_buffer_len = remaining_len;
                    at_buffer[at_buffer_len] = '\0';
                } else {
                    at_buffer_len = 0;
                    at_buffer[0] = '\0';
                }
            } else {
                ESP_LOGE(TAG, "Buffer length mismatch");
                at_buffer_len = 0;
                memset(at_buffer, 0, sizeof(at_buffer));
                break;
            }
        }
    }
}
static void factory_test_aging_task(void *pvParameters) {
    // 设置初始音量
    user_set_volume(80);
    
    // 定义播放间隔时间
    const TickType_t xDelay = 10 * 1000 / portTICK_PERIOD_MS;
    
    while (1) {
        // 播放提示音
        ESP_LOGI(TAG, "Playing aging test tone");
        Application::GetInstance().PlaySound(Lang::Sounds::P3_TEST_MODE);
        // audio_tone_play("spiffs://spiffs/under_aging.mp3");
        
        // 等待指定时间
        vTaskDelay(xDelay);
    }
}

// 启动老化测试任务
void factory_start_aging_task(void) {
    // 创建老化测试任务
    xTaskCreate(factory_test_aging_task, "factory_test_aging_task", 1024*4, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Aging test task started");
}

#endif 