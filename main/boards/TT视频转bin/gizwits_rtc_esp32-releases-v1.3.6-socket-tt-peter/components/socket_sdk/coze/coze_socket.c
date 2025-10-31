#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "esp_log.h"
#include "audio_processor.h"
#include "board/gpio.h"
#include "config.h"
#include "esp_peripherals.h"
#include "cJSON.h"
#include "error_monitor.h"
#include "audio_recorder.h"
#include "recorder_sr.h"
#include <stdbool.h>
#include "i2s_stream.h"
#include "coze_socket.h"
#include "board/charge.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "esp_vad.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "sdk_api.h"
#include "unit/tool.h"
#include "es8311.h"
#include "audio_log.h"
#include "esp_check.h"
#include "es7210.h"

#include "mqtt.h"
#include "uart_ctrl_lcd.h"
#include "tt_ledc.h"
#include "hall_switch.h"
#include "mqtt.h"

static const char* TAG = "COZE_SOCKET";

#define OPUS_FRAME_SIZE_MS 60
// 16000 码率 2  48000 码率 6
#define OPUS_FRAME_SIZE 2 * OPUS_FRAME_SIZE_MS

#ifdef CONFIG_TEST_MODE_VOICE
static char *last_transcript_message;
#endif

static uint32_t last_command_time = 0;  // 上次命令触发时间
static char last_command[32] = {0};     // 上次触发的命令
#define DEBOUNCE_TIME_MS 2000

static volatile uint32_t last_audio_time = 1; // 开机就判断
static volatile bool voice_sleep_flag = false;
static volatile bool agent_is_pushing = false;
static volatile bool ws_connected = false;
static volatile bool is_playing_cache = false;
static esp_websocket_client_handle_t socket_client = NULL;
static volatile bool user_speaking_flag = false;
static volatile bool discard_first_i2s_data = false;

void break_rec_with_key(void);
void start_recorder_with_key(void);
void esp_websocket_client_destroy_task(void *pvParameters);
void vad_is_detect_task(void *pvParameters);

// 添加这些全局变量来存储 WebSocket 配置字符串
static char ws_uri[128+64] = {0};
static char ws_headers[512] = {0};

// 添加初始化信号量
// static SemaphoreHandle_t ws_init_mutex = NULL;
static bool ws_initializing = false;

static volatile uint32_t coze_conversation_id = 0;
uint32_t get_coze_conversation_id(void) {
    return coze_conversation_id;
}

// 添加一个全局变量来跟踪 WebSocket 客户端状态
static volatile bool ws_client_destroying = false;
static void cleanup_websocket_task(void *pvParameters);
void clear_audio_buffer(void);

// 添加全局变量，记录上次播放网络错误提示音的时间
static uint32_t last_network_error_play_time = 0;

static bool need_wellcome_voice = true;

static bool manual_break_flag = false;
static uint32_t last_set_manual_break_time = 0;

static bool s_first_audio_after_connect = true;

// 添加缓冲区阈值定义
#define BUFFER_PLAY_THRESHOLD 0.2f  // 10% 的缓冲区阈值
#define BUFFER_PLAY_FIRST_THRESHOLD 0.3f  // 10% 的缓冲区阈值

static float get_buffer_play_threshold(void) {
    if (s_first_audio_after_connect) {
        return BUFFER_PLAY_FIRST_THRESHOLD;
    } else {
        return BUFFER_PLAY_THRESHOLD;
    }
}

static void set_first_audio_after_connect(bool flag) {
    s_first_audio_after_connect = flag;
}

bool get_manual_break_flag(void) {
    // 3s 后自动取消手动忽略
    if (esp_timer_get_time() / 1000 - last_set_manual_break_time > 3000) {
        send_trace_log("手动忽略 manual_break_flag", "3s 后自动取消");
        manual_break_flag = false;
    }
    return manual_break_flag;
}

void set_manual_break_flag(bool flag) {
    ESP_LOGI(TAG, "%s: %d", __func__, flag);

    manual_break_flag = flag;
    last_set_manual_break_time = esp_timer_get_time() / 1000;
}

// 检查WebSocket连接状态
bool is_websocket_connected(void) {
    return ws_connected;
}

bool set_websocket_connected(bool connected) {
    ws_connected = connected;
    if (connected) {
        set_first_audio_after_connect(true);
    }
}

bool should_discard_first_i2s_data(void) {
    if (discard_first_i2s_data) {
        discard_first_i2s_data = false;
        return true;
    }
    return false;
}


esp_websocket_client_handle_t get_socket_client(void) {
    // static SemaphoreHandle_t socket_client_mutex = NULL;

    // if (socket_client_mutex == NULL) {
    //     socket_client_mutex = xSemaphoreCreateMutex();
    //     if (socket_client_mutex == NULL) {
    //         ESP_LOGE(TAG, "Failed to create mutex for socket client");
    //         return NULL;
    //     }
    // }

    // if (xSemaphoreTake(socket_client_mutex, portMAX_DELAY) == pdTRUE) {
    //     esp_websocket_client_handle_t client = socket_client;
    //     xSemaphoreGive(socket_client_mutex);
    //     return client;
    // } else {
    //     ESP_LOGE(TAG, "Failed to take mutex for socket client");
    //     return NULL;
    // }
    return socket_client;
}

bool get_voice_sleep_flag(void) {
    return voice_sleep_flag;
}

void __set_voice_sleep_flag(const char *funN, uint32_t line, bool flag) {
    ESP_LOGI(TAG, "%s flag:%s by %s, %d", __func__, flag ? "true" : "false", funN, line);

    send_trace_log("设置语音睡眠状态", flag ? "true" : "false");
    voice_sleep_flag = flag;
}
bool get_is_playing_cache(void) {
    return is_playing_cache;
}

bool get_user_speaking_flag(void) {
    return user_speaking_flag;
}

void set_user_speaking_flag(bool flag) {
    send_trace_log("设置用户说话状态", flag ? "true" : "false");
    user_speaking_flag = flag;
}


void set_gain_task(int gain) {
    ESP_LOGI(TAG, "[CZID:%u] set_gain_task: %d", get_coze_conversation_id(), gain);
    
#if defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    es7210_adc_set_gain(ES7210_INPUT_MIC1, gain);
#else
    es8311_set_mic_gain(gain);
#endif

    vTaskDelete(NULL);
}


void set_is_playing_cache(bool flag) {
    is_playing_cache = flag;
    if (flag) {
        ESP_LOGI(TAG, "[CZID:%u] set_is_playing_cache: true", get_coze_conversation_id());
        update_i2s_last_playing_time();
        // lcd_state_event_send(EVENT_REPLY);
    } else {
        ESP_LOGI(TAG, "[CZID:%u] set_is_playing_cache: false", get_coze_conversation_id());
        // 清空音频缓冲区
        clear_audio_buffer();
    }

#if defined(CONFIG_AUDIO_BOARD_ATOM_V1_2) || defined(CONFIG_AUDIO_BOARD_TOYCORE_V1) || defined(CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE) || defined(CONFIG_AUDIO_BOARD_TT_MUSIC_V1)
    if (!flag) {
        // 停止播放时，恢复麦克风增益
        ESP_LOGI(TAG, "[CZID:%u] es8311: set mic gain to normal", get_coze_conversation_id());
        // es8311_set_mic_gain(AUDIO_MIC_GAIN_NORMAL);
        // vTaskDelay(300);
        xTaskCreate(set_gain_task, "set_gain_task", 1024 * 4, AUDIO_MIC_GAIN_NORMAL, 15, NULL);
    } else {
        // 停止播放时，恢复麦克风增益
        ESP_LOGI(TAG, "[CZID:%u] es8311: set mic gain to less sensitive", get_coze_conversation_id());
        // es8311_set_mic_gain(AUDIO_MIC_GAIN_LESS_SENSITIVE);
        // vTaskDelay(300);
        xTaskCreate(set_gain_task, "set_gain_task", 1024 * 4, AUDIO_MIC_GAIN_LESS_SENSITIVE, 15, NULL);
    }
#endif
}

uint32_t get_last_audio_time(void) {
    return last_audio_time;
}

void set_last_audio_time(uint32_t time) {
    last_audio_time = time;
}

// 在播放缓冲区结构中添加已播放帧计数
static struct {
    ringbuf_handle_t rb;
    SemaphoreHandle_t mutex;
    struct {
        uint32_t total_writes;
        uint32_t total_reads;
        size_t bytes_written;
        size_t bytes_read;
        size_t max_size;
        uint32_t buffer_full_count;
        float buffer_usage;
        struct {
            uint32_t frame_count;      // 总帧数
            uint32_t played_frames;    // 已播放帧数
            uint32_t total_opus_bytes;
            float avg_frame_size;
        } opus_stats;
    } stats;
} play_buffer = {NULL, NULL, {0}};

static bool recording_flag = true;
bool get_recording_flag(void) {
    return recording_flag;
}


static void voice_read_task(void *pvParameters);
static void voice_send_task(void *pvParameters);
void init_websocket();

// 在文件开头添加音频播放缓冲区相关定义
#define AUDIO_PLAY_BUFFER_SIZE (1024 * 1024)
#define PLAYER_RATE 16000

// 添加播放相关的宏定义
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
#define PACKAGE_MS 60
#define PLAY_CHUNK_SIZE 6 * (PACKAGE_MS + 20) + 2  // + 2 是长度信息
#elif defined (CONFIG_AUDIO_SUPPORT_PCM_DECODER)
#define PACKAGE_MS 10
#define PLAY_CHUNK_SIZE 320                  // 每次播放的数据大小 (16bit, 16kHz, 10ms = 16000*2*0.01)
#else
#define PACKAGE_MS 10
#define PLAY_CHUNK_SIZE 320                 // 默认每次播放的数据大小
#endif

#define RECORDER_READ_SIZE 640
#define VAD_FRAME_LENGTH_MS 20
#define VAD_BUFFER_LENGTH (VAD_FRAME_LENGTH_MS * PLAYER_RATE / 1000)

// 添加音频缓冲区相关定义
#define AUDIO_BUFFER_MS 500  // 缓冲800ms的音频
#define AUDIO_BUFFER_SIZE (PLAYER_RATE * 2 * AUDIO_BUFFER_MS / 1000)

// void vad_is_detect_task(void *pvParameters);
// 音频缓冲区结构
typedef struct {
    uint8_t *buffer;
    size_t size;          // 缓冲区总大小
    size_t write_pos;     // 写入位置
    size_t valid_size;    // 当前有效数据大小
} audio_ring_buffer_t;

static volatile bool wakeup_flag = true;

// 上电默认启动
static audio_ring_buffer_t audio_buffer = {0};

bool get_wakeup_flag(void) {
    return wakeup_flag;
}

void set_wakeup_flag(bool flag) {
    wakeup_flag = flag;
}

void set_recording_flag(bool flag) {
    send_trace_log("设置录音状态", flag ? "true" : "false");
    ESP_LOGI(TAG, "set_recording: %s", flag ? "true" : "false");
    recording_flag = flag;
}

static void speech_recognition_result(const char* command, float score) {
    // ESP_LOGE(TAG, "Speech recognition result: %s (score: %.2f)", command, score);
    
    // 获取当前时间
    uint32_t current_time = esp_timer_get_time() / 1000;  // 转换为毫秒

    // 获取配置的唤醒词
    const char* wake_word = 
#if CONFIG_WAKE_UP_HAIMIANBAOBAO
        "haimianbaobao"
#elif CONFIG_WAKE_UP_MOFANGMOFANG
        "mofangmofang"
#elif CONFIG_WAKE_UP_NONE
        ""
#endif
        ;

    // 根据识别结果执行相应操作
    if (strcmp(command, wake_word) == 0) {
        // 唤醒
        // 检查是否是相同命令且在防抖时间内
        if (strcmp(command, last_command) == 0 && 
            (current_time - last_command_time) < DEBOUNCE_TIME_MS) {
            ESP_LOGI(TAG, "Command debounced: %s", command);
            return;
        } else {
            // 更新上次命令信息
            strncpy(last_command, command, sizeof(last_command) - 1);
            last_command_time = current_time;
            ESP_LOGE(TAG, "Speech recognition result: %s (score: %.2f)", command, score);
            // xTaskCreate(cancel_ai_agent_task, "cancel_ai_agent_task", 1024 * 4, "speech_recognition_result", 5, NULL);
            send_conversation_chat_cancel("speech_recognition_result");
        }
    } 
     
}

// 初始化音频缓冲区
static void init_audio_ring_buffer() {
    audio_buffer.buffer = audio_calloc(1, AUDIO_BUFFER_SIZE);
    audio_buffer.size = AUDIO_BUFFER_SIZE;
    audio_buffer.write_pos = 0;
    audio_buffer.valid_size = 0;
}

// 写入数据到环形缓冲区
static void audio_buffer_write(const uint8_t *data, size_t len) {
    if (!audio_buffer.buffer || !data || len == 0) return;

    // 确保不会写入超过缓冲区大小的数据
    if (len > audio_buffer.size) {
        data += (len - audio_buffer.size);
        len = audio_buffer.size;
    }

    size_t first_write = MIN(len, audio_buffer.size - audio_buffer.write_pos);
    memcpy(audio_buffer.buffer + audio_buffer.write_pos, data, first_write);

    // 如果数据需要回环写入
    if (first_write < len) {
        memcpy(audio_buffer.buffer, data + first_write, len - first_write);
        audio_buffer.write_pos = len - first_write;
    } else {
        audio_buffer.write_pos = (audio_buffer.write_pos + first_write) % audio_buffer.size;
    }

    // 更新有效数据大小
    audio_buffer.valid_size = MIN(audio_buffer.valid_size + len, audio_buffer.size);
}

// 获取并清空缓冲区数据
static size_t audio_buffer_get_and_clear(uint8_t *out_data) {
    if (!audio_buffer.buffer || !out_data || audio_buffer.valid_size == 0) return 0;

    size_t read_pos = (audio_buffer.write_pos + audio_buffer.size - audio_buffer.valid_size) % audio_buffer.size;
    size_t first_read = MIN(audio_buffer.valid_size, audio_buffer.size - read_pos);

    // 读取第一部分数据
    memcpy(out_data, audio_buffer.buffer + read_pos, first_read);

    // 如果需要回环读取
    if (first_read < audio_buffer.valid_size) {
        memcpy(out_data + first_read, audio_buffer.buffer, audio_buffer.valid_size - first_read);
    }

    size_t total_read = audio_buffer.valid_size;
    audio_buffer.valid_size = 0;  // 清空缓冲区
    return total_read;
}

void clear_audio_buffer(void) {
    ESP_LOGI(TAG, "clear_audio_buffer");
    audio_buffer.valid_size = 0;  // 清空缓冲区
}

// opus数据缓存相关定义
static struct {
    char *buffer;
    size_t len;
    size_t capacity;
} opus_cache = {NULL, 0, 0};

// 初始化opus缓存函数声明
static void init_opus_cache(void);

// 初始化opus缓存函数实现
static void init_opus_cache() {
    opus_cache.capacity = 8192;
    opus_cache.buffer = audio_calloc(1, opus_cache.capacity);
    opus_cache.len = 0;
}

// 添加消息队列相关定义
typedef struct {
    char *data;
    int len;
} audio_data_t;

#define AUDIO_QUEUE_LENGTH 30
#define AUDIO_PROCESS_DELAY_MS 20  // 处理间隔

static QueueHandle_t audio_data_queue = NULL;
static QueueHandle_t voice_queue = NULL;

// 添加WebSocket数据队列相关定义
#define WS_QUEUE_LENGTH 300
#define VOICE_QUEUE_LENGTH 20

// 添加数据包状态定义
typedef enum {
    WS_DATA_START,
    WS_DATA_CONTINUE,
    WS_DATA_END,
    WS_DATA_TEXT
} ws_data_type_t;

// 修改WebSocket数据结构
typedef struct {
    char *data;
    size_t len;
    ws_data_type_t type;
    bool is_final;
} ws_data_t;

// 添加临时缓冲区来存储分片数据
static struct {
    char *buffer;
    size_t len;
    size_t capacity;
    bool receiving;
} ws_buffer = {NULL, 0, 0, false};

typedef struct {
    char *data;
    size_t len;
} voice_data_t;

// 初始化WebSocket缓冲区
static void init_ws_buffer() {
    ws_buffer.capacity = 32768;  // 32KB 初始容量
    ws_buffer.buffer = malloc(ws_buffer.capacity);
    ws_buffer.len = 0;
    ws_buffer.receiving = false;
}

static size_t play_buffer_write(const char *data, size_t len);
// 添加数据到WebSocket缓冲区
static void append_to_ws_buffer(const char *data, size_t len) {
    // 检查是否需要扩容
    if (ws_buffer.len + len > ws_buffer.capacity) {
        ws_buffer.capacity = (ws_buffer.len + len) * 2;
        ws_buffer.buffer = realloc(ws_buffer.buffer, ws_buffer.capacity);
    }
    
    // 添加新数据
    memcpy(ws_buffer.buffer + ws_buffer.len, data, len);
    ws_buffer.len += len;
}

struct volc_t {
    recorder_pipeline_handle_t record_pipeline;
    player_pipeline_handle_t   player_pipeline;
    void                      *recorder_engine;
    QueueHandle_t              frame_q;
    esp_dispatcher_handle_t    esp_dispatcher;
    bool                       data_proc_running;
    sdk_actived_mode_t         actived_mode;
};

static struct volc_t s_volc;


static QueueHandle_t ws_data_queue = NULL;
// 添加一个标志来表示 agent 是否正在说话
static esp_err_t open_audio_pipeline();
void end_recorder();
void start_recorder();

// 保存 WebSocket 连接参数
static rtc_params_t ws_params = {0};

typedef struct {
    char *frame_ptr;
    int frame_len;
} frame_package_t;

bool get_need_wellcome_voice(void) {
    // 带唤醒词不要欢迎语
    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP || 
        s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP) {
        ESP_LOGI(TAG, "actived_mode: %d, welcome: false", s_volc.actived_mode);
        return false;
    }
    ESP_LOGI(TAG, "actived_mode: %d, welcome: %d", s_volc.actived_mode, need_wellcome_voice);
    return need_wellcome_voice;
}

void set_need_wellcome_voice(bool flag) {
    need_wellcome_voice = flag;
}


static void esp_dump_per_task_heap_info(void);
static esp_err_t open_audio_pipeline()
{
    send_trace_log("打开音频管道", "");
    s_volc.record_pipeline = recorder_pipeline_open(s_volc.actived_mode);
    s_volc.player_pipeline = player_pipeline_open();
    recorder_pipeline_run(s_volc.record_pipeline);
    player_pipeline_run(s_volc.player_pipeline);
    send_trace_log("打开音频管道结束", "");
    return ESP_OK;
}

static esp_err_t rec_engine_cb(audio_rec_evt_t *event, void *user_data)
{
    static bool is_first_wakeup = true;
    if ((s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) || (s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP)) {
        if (AUDIO_REC_WAKEUP_START == event->type) {
            // Wakeup的AFE初始化后会触发一次WAKEUP_START事件，所以需要忽略
            if (is_first_wakeup) {
                is_first_wakeup = false;
                return ESP_OK;
            }
            
            if(get_hall_state() == HALL_STATE_OFF)
            {
                ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_WAKEUP_START, bu HALL_STATE_OFF, do nothing");
                return ESP_OK;
            }


            send_trace_log("触发唤醒词模式，开始录音", "");
            set_last_audio_time(esp_timer_get_time());
            ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_WAKEUP_START");
         
            if (is_playing_cache || s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP) {
                // 打断
                send_conversation_chat_cancel("rec_engine_cb");
                // xTaskCreate(cancel_ai_agent_task, "cancel_ai_agent_task", 1024 * 4, "rec_engine_cb", 5, NULL);
                clear_audio_buffer();
            }

            user_event_notify(USER_EVENT_WAKEUP);
            user_event_notify(USER_EVENT_STANDBY);
            // wakeup_flag = true;
            // SET_WAKEUP_FLAG(true);
        } else if (AUDIO_REC_VAD_START == event->type) {
            ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_VAD_START wakeup_flag=%d", wakeup_flag);
            if (
                (s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP) &&
                 (recording_flag == false) && (is_playing_cache == false) &&
                 get_audio_url_is_playing() == false
                ) {
                if (wakeup_flag) {
                    // 只有在唤醒状态下，才启动对话
                    xTaskCreate(vad_is_detect_task, "vad_is_detect_task", 1024 * 4, NULL, 5, NULL);
                }
            }
            // else
            // {
            //     ESP_LOGI(TAG, "actived_mode=%d, recording_flag=%d, is_playing_cache=%d, audio_url_is_playing=%d", 
            //          s_volc.actived_mode, recording_flag, is_playing_cache, get_audio_url_is_playing());
            // }
            
        } else if (AUDIO_REC_VAD_END == event->type) {
            ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_VAD_END");
            if ((s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP) && (recording_flag == true)) {
                xTaskCreate(end_recorder_task, "end_recorder_task", 1024 * 4, NULL, 5, NULL);
            }
        } else if (AUDIO_REC_WAKEUP_END == event->type) {
            if (voice_sleep_flag) {
                // wakeup_flag = false;
                // SET_WAKEUP_FLAG(false);
                run_sleep();
                ESP_LOGI(TAG, "rec_engine_cb - AUDIO_REC_WAKEUP_END");
                send_trace_log("唤醒词模式结束，运行睡眠", "");
            }
        } else {
        }
    }
    return ESP_OK;
}
// 添加文本缓冲区结构定义
static struct {
    char *buffer;
    size_t len;
    size_t capacity;
} text_buffer = {NULL, 0, 0};

// 初始化缓冲区
static void init_text_buffer() {
    text_buffer.capacity = 32768 * 2;  // 64KB 初始容量
    text_buffer.buffer = malloc(text_buffer.capacity);
    text_buffer.len = 0;
}

// 清理缓冲区
static void clear_text_buffer() {
    text_buffer.len = 0;
}

// 添加数据到缓冲区
static void append_to_buffer(const char *data, size_t len) {
    // 检查是否需要扩容
    if (text_buffer.len + len > text_buffer.capacity) {
        text_buffer.capacity = (text_buffer.len + len) * 2;
        text_buffer.buffer = realloc(text_buffer.buffer, text_buffer.capacity);
    }
    
    // 添加新数据
    memcpy(text_buffer.buffer + text_buffer.len, data, len);
    text_buffer.len += len;
    text_buffer.buffer[text_buffer.len] = '\0';  // 确保字符串结束
}

void send_conversation_chat_cancel(const char *para)
{
    // 使用静态变量避免栈溢出
    send_trace_log("运行打断 AI", "");
    set_manual_break_flag(true);
    stop_audio(true);

    if (s_volc.actived_mode != SDK_ACTIVED_MODE_SERVER_VAD && s_volc.actived_mode != SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) 
    {
        xTaskCreate(cancel_ai_agent_task, "cancel_ai_agent_task", 1024 * 4, para, 5, NULL);
    }
}

void cancel_ai_agent_task( void * para) {


    static const char *cancel_message_format = "{"
        "\"id\":\"%s\","
        "\"event_type\":\"conversation.chat.cancel\""
    "}";
    static char cancel_message[1024];
    static char event_id[32];

    // 生成事件ID
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());
    
    // 构建取消消息
    snprintf(cancel_message, sizeof(cancel_message), cancel_message_format, event_id);
    
    // 发送取消消息
    if (is_websocket_connected()) {
        esp_err_t ret = esp_websocket_client_send_text(socket_client, cancel_message, strlen(cancel_message), 4000);
        if (ret > 0) {
            send_trace_log("发送打断 AI 成功", "");
            ESP_LOGI(TAG, "Cancel message sent successfully by %s", para == NULL?"NULL":(char*)para);
        } else {
            send_trace_log("发送打断 AI 失败", "");
            ESP_LOGE(TAG, "Failed to send cancel message: %d by %s", ret, para == NULL?"NULL":(char*)para);
        }
    }
    send_trace_log("运行打断 AI 结束", "");

    vTaskDelete(NULL);
}
// 停止音频播放
void stop_audio(bool stop_pipeline) {
    send_trace_log("运行停止音频播放", "");
    // 停止播放
    set_is_playing_cache(false);
    agent_is_pushing = false;
    
    // 清空播放缓冲区
    if (play_buffer.rb) {
        send_trace_log("清空播放缓冲区", "");
        xSemaphoreTake(play_buffer.mutex, portMAX_DELAY);
        rb_reset(play_buffer.rb);  // 重置环形缓冲区
        memset(&play_buffer.stats, 0, sizeof(play_buffer.stats));  // 清零统计信息
        xSemaphoreGive(play_buffer.mutex);
    }

    // player_pipeline_clean(s_volc.player_pipeline);
    
    // 清理opus缓存
    #if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    if (opus_cache.buffer) {
        send_trace_log("清理opus缓存", "");
        opus_cache.buffer = audio_realloc(opus_cache.buffer, opus_cache.capacity);
    }
    #endif

    audio_tone_stop();
    // audio_tone_url_stop();
    if (stop_pipeline) {
        send_trace_log("停止音频管道", "");
        player_pipeline_stop(s_volc.player_pipeline);
    }
}

void send_hello_message() {
    send_trace_log("发送 心跳 消息", "");
    // 发送初始化消息
    const char *init_message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"(用户已经1分钟没理你了)\""
        "}"
    "}";
    
    esp_err_t ret = esp_websocket_client_send_text(socket_client, init_message, strlen(init_message), 4000);
    if (ret > 0) {
        ESP_LOGI(TAG, "Init message sent successfully");
        send_trace_log("发送 心跳 消息成功", "");
    } else {
        ESP_LOGE(TAG, "Failed to send init message: %d", ret);
        send_trace_log("发送 心跳 消息失败", "");
    }
}

// 深度合并两个 cJSON 对象
static void cjson_deep_merge(cJSON *dest, cJSON *src) {
    if (!dest || !src) return;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src) {
        cJSON *existing = cJSON_GetObjectItem(dest, item->string);
        if (existing) {
            // 如果目标对象中已存在该键
            if (cJSON_IsObject(item) && cJSON_IsObject(existing)) {
                // 如果是对象，递归合并
                cjson_deep_merge(existing, item);
            } else {
                // 其他情况，替换值
                cJSON_DeleteItemFromObject(dest, item->string);
                cJSON_AddItemToObject(dest, item->string, cJSON_Duplicate(item, 1));
            }
        } else {
            // 如果目标对象中不存在该键，直接添加
            cJSON_AddItemToObject(dest, item->string, cJSON_Duplicate(item, 1));
        }
    }
}

void send_init_config() {
    // 使用时间戳作为 event_id
    send_trace_log("发送初始化配置", "");

    char event_id[32];
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());

    // 创建根对象
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create root JSON object");
        return;
    }

    // 添加基本字段
    cJSON_AddStringToObject(root, "id", event_id);
    cJSON_AddStringToObject(root, "event_type", "chat.update");

    // 创建 data 对象
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to create data JSON object");
        return;
    }
    cJSON_AddItemToObject(root, "data", data);

    // 创建 chat_config 对象
    cJSON *chat_config = cJSON_CreateObject();
    if (!chat_config) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to create chat_config JSON object");
        return;
    }
    cJSON_AddItemToObject(data, "chat_config", chat_config);

    // 添加 chat_config 字段
    cJSON_AddBoolToObject(chat_config, "auto_save_history", true);
    cJSON_AddStringToObject(chat_config, "conversation_id", ws_params.conv_id);
    cJSON_AddStringToObject(chat_config, "user_id", ws_params.user_id);
    cJSON_AddItemToObject(chat_config, "meta_data", cJSON_CreateObject());
    cJSON_AddItemToObject(chat_config, "custom_variables", cJSON_CreateObject());
    cJSON_AddItemToObject(chat_config, "extra_params", cJSON_CreateObject());

    // 添加 need_play_prologue
    cJSON_AddBoolToObject(data, "need_play_prologue", get_need_wellcome_voice());

    // 根据 actived_mode 添加 turn_detection
    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || 
        s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        cJSON *turn_detection = cJSON_CreateObject();
        if (turn_detection) {
            cJSON_AddStringToObject(turn_detection, "type", "server_vad");
            cJSON_AddNumberToObject(turn_detection, "prefix_padding_ms", 500);
            cJSON_AddNumberToObject(turn_detection, "silence_duration_ms", 500);
            cJSON_AddItemToObject(data, "turn_detection", turn_detection);
        }
    }

    // 创建 input_audio 对象
    cJSON *input_audio = cJSON_CreateObject();
    if (input_audio) {
        cJSON_AddStringToObject(input_audio, "format", "pcm");
        cJSON_AddStringToObject(input_audio, "codec", "opus");
        cJSON_AddNumberToObject(input_audio, "sample_rate", 16000);
        cJSON_AddNumberToObject(input_audio, "channel", 1);
        cJSON_AddNumberToObject(input_audio, "bit_depth", 16);
        cJSON_AddItemToObject(data, "input_audio", input_audio);
    }

    // 创建 output_audio 对象
    cJSON *output_audio = cJSON_CreateObject();
    if (output_audio) {
        cJSON_AddStringToObject(output_audio, "codec", 
            #if defined (CONFIG_AUDIO_SUPPORT_PCM_DECODER)
            "pcm"
            #else
            "opus"
            #endif
        );

        #if defined (CONFIG_AUDIO_SUPPORT_PCM_DECODER)
        cJSON *pcm_config = cJSON_CreateObject();
        if (pcm_config) {
            cJSON_AddNumberToObject(pcm_config, "sample_rate", 16000);
            cJSON_AddItemToObject(output_audio, "pcm_config", pcm_config);
        }
        #else
        cJSON *opus_config = cJSON_CreateObject();
        if (opus_config) {
            cJSON_AddNumberToObject(opus_config, "bitrate", 16000);
            cJSON_AddBoolToObject(opus_config, "use_cbr", true);
            cJSON_AddNumberToObject(opus_config, "frame_size_ms", OPUS_FRAME_SIZE_MS);
            
            cJSON *limit_config = cJSON_CreateObject();
            if (limit_config) {
                cJSON_AddNumberToObject(limit_config, "period", 1);
                cJSON_AddNumberToObject(limit_config, "max_frame_num", 60);
                cJSON_AddItemToObject(opus_config, "limit_config", limit_config);
            }
            
            cJSON_AddItemToObject(output_audio, "opus_config", opus_config);
        }
        #endif

        cJSON_AddNumberToObject(output_audio, "speech_rate", 0);
        cJSON_AddStringToObject(output_audio, "voice_id", ws_params.voice_id);
        cJSON_AddItemToObject(data, "output_audio", output_audio);
    }


    // 如果存在config，深度合并到data中
    if (ws_params.config) {
        cjson_deep_merge(data, ws_params.config);
    }

    // 将 JSON 对象转换为字符串
    char *json_string = cJSON_PrintUnformatted(root);
    if (!json_string) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to print JSON");
        return;
    }

    ESP_LOGI(TAG, "init_config_message: %s", json_string);
    esp_err_t ret = esp_websocket_client_send_text(socket_client, json_string, strlen(json_string), 2000);
    
    // 释放内存
    free(json_string);
    cJSON_Delete(root);

    if (ret > 0) {
        send_trace_log("发送初始化配置成功", "");
        // set_need_wellcome_voice(false);
        ESP_LOGI(TAG, "Sent init config message with event_id: %s", event_id);
    } else {
        send_trace_log("发送初始化配置失败", "");
        ESP_LOGE(TAG, "Failed to send init config message: %d", ret);
    }
}


uint64_t last_audio_packet_time = 0;

// 修改重连函数，使用消息队列或事件来触发重连
static EventGroupHandle_t ws_event_group;
#define WS_RECONNECT_BIT BIT0

void re_connect_websocket(void) {
    send_trace_log("启动 Socket 重连", "");

    // 检查是否正在初始化
    if (ws_initializing) {
        ESP_LOGW(TAG, "WebSocket 初始化中，跳过重连");
            return;
        }
    
    // 不直接调用清理和初始化，而是发送事件
    xEventGroupSetBits(ws_event_group, WS_RECONNECT_BIT);
}

// 添加重连任务
static void ws_reconnect_task(void *pvParameters) {
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(ws_event_group,
                                             WS_RECONNECT_BIT,
                                             pdTRUE,
                                             pdFALSE,
                                             portMAX_DELAY);
                                             
        if (bits & WS_RECONNECT_BIT) {
            // 确保在主任务中执行清理
            if (socket_client) {
                send_trace_log("运行 Socket 清理", "");
                esp_websocket_client_stop(socket_client);
                vTaskDelay(pdMS_TO_TICKS(100));  // 等待停止完成
                esp_websocket_client_destroy(socket_client);
                socket_client = NULL;
                set_websocket_connected(false);
            }
            
            // 重置所有状态
            set_recording_flag(false);
            agent_is_pushing = false;
            ws_buffer.len = 0;
            ws_buffer.receiving = false;
            
            // 等待一段时间再重连
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // 初始化新的连接
            init_websocket();
        }
    }
}

static void process_websocket_data(const char *data, size_t len) {
    if (!data || len == 0) {
        return;
    }

    // 按长度拷贝字符
    char *cut_data = (char *)malloc(len + 1);
    if (!cut_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for cut_data");
        return;
    }
    strncpy(cut_data, data, len);
    cut_data[len] = '\0';

    // 查找事件类型
    const char *event_start = strstr(cut_data, "\"event_type\":\"");
    if (!event_start) {
        free(cut_data);
        return;
    }
    
    event_start += strlen("\"event_type\":\"");
    
    // 找到事件类型结束的引号位置
    const char *event_end = strchr(event_start, '"');
    if (!event_end) {
        free(cut_data);
        return;
    }
    
    // 计算事件类型的长度并打印
    size_t event_len = event_end - event_start;
    
    if (event_len == 0 || event_len >= 64) {
        free(cut_data);
        return;
    }
    
    // 复制事件类型
    char event_type[64] = {0};
    strncpy(event_type, event_start, event_len);
    event_type[event_len] = '\0';

    if (strcmp(event_type, "conversation.audio.delta") == 0) {
        if (get_manual_break_flag() || get_hall_state() == HALL_STATE_OFF || get_valuestate() == state_VALUE0_close
            || get_valuestate() == state_VALUE1_standby || audio_tone_url_is_playing()) {
            // ESP_LOGI(TAG, "manual_break_flag ignore audio");
            free(cut_data);
            return;
        }
        // if (recording_flag == true) {
        //     ESP_LOGI(TAG, "recording_flag ignore audio");
        //     return;
        // }
        // 查找 content 字段
        const char *content_start = strstr(cut_data, "\"content\":\"");
        if (content_start) {
            content_start += strlen("\"content\":\"");
            
            // 找到 content 结束的位置 (下一个引号，但要考虑转义)
            const char *content_end = content_start;
            bool escaped = false;
            
            while (*content_end) {
                if (*content_end == '\\') {
                    escaped = !escaped;
                } else if (*content_end == '"' && !escaped) {
                    break;
                } else {
                    escaped = false;
                }
                content_end++;
            }
            
            if (*content_end == '"') {
                size_t content_len = content_end - content_start;
                size_t out_len;
                unsigned char *decoded = base64_decode(content_start, content_len, &out_len);
                // ESP_LOGI(TAG, "[CZID:%u] conversation.audio.delta len:%d", get_coze_conversation_id(), out_len);
                printf("^%d", out_len);
                
                if (decoded) {
                    if (!agent_is_pushing) {
                        ESP_LOGI(TAG, "agent_is_pushing: filled=%d", rb_bytes_filled(play_buffer.rb));
                        discard_first_i2s_data = true;
                    }

                    #if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
                        // 确保缓存足够大
                        if (opus_cache.capacity < out_len + 2) {
                            opus_cache.capacity = out_len + 2;
                            opus_cache.buffer = audio_realloc(opus_cache.buffer, opus_cache.capacity);
                        }
                        
                        // 添加2字节的长度前缀
                        opus_cache.buffer[0] = (out_len >> 8) & 0xFF;
                        opus_cache.buffer[1] = out_len & 0xFF;
                        
                        // 复制opus数据
                        memcpy(opus_cache.buffer + 2, decoded, out_len);
                        
                        size_t write_result = play_buffer_write(opus_cache.buffer, out_len + 2);
                        if (write_result < out_len + 2) {
                            ESP_LOGW(TAG, "Incomplete write: %d/%d", write_result, out_len + 2);
                        }
                    #elif defined (CONFIG_AUDIO_SUPPORT_PCM_DECODER)
                        size_t write_result = play_buffer_write((char *)decoded, out_len);
                        if (write_result < out_len) {
                            ESP_LOGW(TAG, "Incomplete PCM write: %d/%d", write_result, out_len);
                        }
                    #endif

                    free(decoded);

                    if (!agent_is_pushing) {
                        ESP_LOGI(TAG, "agent_is_pushing: true");
                        agent_is_pushing = true;
                    }
                }
            }
        }
    } else if (strcmp(event_type, "conversation.audio.completed") == 0) {
        agent_is_pushing = false;
        send_trace_log("Coze 返回音频完成", "");
        ESP_LOGI(TAG, "[CZID:%u] conversation.audio.completed", get_coze_conversation_id());
#ifdef CONFIG_TEST_MODE_VOICE
        decrement_recorder_counter();
#endif

    } else if (strcmp(event_type, "chat.created") == 0) {
        ESP_LOGI(TAG, "[CZID:%u] chat.created", get_coze_conversation_id());
        send_trace_log("Coze 返回创建会话成功", "");
#if defined (CONFIG_SUPPORT_HEART_BEAT)
        static bool is_send_wakeup_message = false;
        if (!is_send_wakeup_message) {
            send_wakeup_message();
            is_send_wakeup_message = true;
        }
#endif
      
    } else if (strcmp(event_type, "conversation.chat.canceled") == 0) {
        ESP_LOGI(TAG, "[CZID:%u] conversation.chat.canceled", get_coze_conversation_id());
        send_trace_log("Coze 返回取消会话成功", "");
        // 这条消息 可能会丢
        set_manual_break_flag(false);
        user_event_notify(USER_EVENT_STANDBY);

    } else if (strcmp(event_type, "conversation.chat.in_progress") == 0) {
        send_trace_log("Coze 返回会话进行中", "");
        set_manual_break_flag(false);
        ESP_LOGI(TAG, "Coze 返回会话进行中");

        user_event_notify(USER_EVENT_CHAT_IN_PROGRESS);


    } else if (strcmp(event_type, "conversation.chat.requires_action") == 0) {
        // ESP_LOGI(TAG, "conversation.chat.requires_action: %s", data);
        handle_requires_action(cut_data);
    }else if (strcmp(event_type, "conversation.chat.failed") == 0) {
        send_trace_log("Coze 返回会话失败", "");
    } else if (strcmp(event_type, "input_audio_buffer.speech_started") == 0) {
        coze_conversation_id++;
        set_user_speaking_flag(true);
        ESP_LOGI(TAG, "[CZID:%u] input_audio_buffer.speech_started: %.300s", get_coze_conversation_id(), cut_data);
        send_trace_log("Coze 返回用户开始说话", "");

        send_conversation_chat_cancel("speech_started");
        // xTaskCreate(cancel_ai_agent_task, "cancel_ai_agent_task", 1024 * 4, "speech_started", 5, NULL);

        if (is_playing_cache) {
            ESP_LOGI(TAG, "input_audio_buffer.speech_started, clean audio cache");
            stop_audio(false);
        } else {
            ESP_LOGI(TAG, "input_audio_buffer.speech_started, no audio cache");
        }
        user_event_notify(USER_EVENT_USER_SPEAKING);
        set_last_audio_time(esp_timer_get_time());
    }  else if (strcmp(event_type, "input_audio_buffer.speech_stopped") == 0) {
        set_user_speaking_flag(false);

        ESP_LOGI(TAG, "[CZID:%u] input_audio_buffer.speech_stoped", get_coze_conversation_id());
        send_trace_log("Coze 返回用户停止说话", "");
        // 用户说完话了 打开管道Error parse url
        player_pipeline_run(s_volc.player_pipeline);
        user_event_notify(USER_EVENT_STANDBY);
        lcd_state_event_send(EVENT_THINK);
        tt_led_strip_set_state(TT_LED_STATE_OFF);

    } else if (strcmp(event_type, "conversation.chat.completed") == 0) {
        ESP_LOGI(TAG, "[CZID:%u] conversation.chat.completed", get_coze_conversation_id());
        
        // 解析JSON数据
        cJSON *root = cJSON_Parse(cut_data);
        if (root) {
            // 获取data对象
            cJSON *data_obj = cJSON_GetObjectItem(root, "data");
            if (data_obj) {
                // 获取usage对象
                cJSON *usage = cJSON_GetObjectItem(data_obj, "usage");
                if (usage) {
                    // 获取token计数
                    cJSON *total = cJSON_GetObjectItem(usage, "token_count");
                    cJSON *output = cJSON_GetObjectItem(usage, "output_count");
                    cJSON *input = cJSON_GetObjectItem(usage, "input_count");
                    
                    if (total && output && input) {
                        // 调用上报函数
                        mqtt_report_token(total->valueint, output->valueint, input->valueint);
                        ESP_LOGI(TAG, "Token usage - Total: %d, Output: %d, Input: %d",
                                total->valueint, output->valueint, input->valueint);
                    }
                }
            }
            // 释放JSON对象
            cJSON_Delete(root);
            // lcd_state_event_send(EVENT_LISTEN);
            // tt_led_strip_set_state(TT_LED_STATE_OFF);
        } else {
            ESP_LOGE(TAG, "Failed to parse completed event JSON");
        }
    } else if (strcmp(event_type, "conversation.audio_transcript.completed") == 0) {
        // ESP_LOGI(TAG, "conversation.audio_transcript.completed: %s", data);

#ifdef CONFIG_TEST_MODE_VOICE
        last_transcript_message = strdup(cut_data);
#endif
    } else if (strcmp(event_type, "error") == 0) {
        ESP_LOGE(TAG, "error: %s", cut_data);

        send_trace_log("Coze 返回错误", cut_data);
        if (strcmp(cut_data, "\"code\":4200") == 0 || strcmp(cut_data, "\"code\":4101") == 0 || strcmp(cut_data, "\"code\":4100") == 0) {
            // 重新获取token
            stop_audio(true);
            // 重新获取token 
            mqtt_get_room_info();
        } else if(strstr(cut_data, "\"code\":717990703") != NULL) {
            // opus 解码失败
            opus_error_check_callback();
        } else {
            stop_audio(true);
            report_error(ERROR_TYPE_NETWORK, ERROR_LEVEL_ERROR,
                        cut_data,
                        NULL);

            // re_connect_websocket();  // 重试
        }
    } else {
        ESP_LOGI(TAG, "[CZID:%u] Unknown event: %s", get_coze_conversation_id(), event_type);
        // send_trace_log("Coze 返回未知事件", event_type);
    }


    // 在函数结束前释放内存
    free(cut_data);
}


// 修改WebSocket事件处理函数
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    static int disconnect_count = 0;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            send_trace_log("ESP WebSocket 连接成功", "");
            ESP_LOGI(TAG, "WebSocket 连接成功");
            disconnect_count = 0;
            set_websocket_connected(true);
            init_ws_buffer();
            send_init_config();
            user_event_notify(USER_EVENT_STANDBY);
            set_last_audio_time(esp_timer_get_time());

            if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
                set_recording_flag(true);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            send_trace_log("ESP WebSocket 断开连接", "");
            ESP_LOGI(TAG, "WebSocket 断开连接");
            set_websocket_connected(false);
            // set_room_info_req_success(0);
            // set_room_info_request_id(xTaskGetTickCount()+get_average_voltage());
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            {
                char error_details[256];
                snprintf(error_details, sizeof(error_details), 
                        "WebSocket error - TLS: 0x%x, Socket: 0x%x",
                        data->error_handle.esp_tls_last_esp_err,
                        data->error_handle.esp_transport_sock_errno);

                // report_error(ERROR_TYPE_NETWORK, ERROR_LEVEL_ERROR,
                //             error_details,
                //             NULL);
                // 添加错误恢复逻辑
                // if (!ws_connected) {
                //     re_connect_websocket();
                // }
                send_trace_log("ESP WebSocket 错误", error_details);
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            // ESP_LOGI(TAG, "WS data: op=%d, fin=%d, len=%d, total_len=%d", 
            //             data->op_code, data->fin, data->data_len, data->payload_len);
            
            switch (data->op_code) {
                case WS_TRANSPORT_OPCODES_TEXT:
                    // 处理文本数据
                    if (data->data_len == data->payload_len) {
                        process_websocket_data(data->data_ptr, data->data_len);
                    } else {
                        // 处理分片数据
                        append_to_ws_buffer(data->data_ptr, data->data_len);
                         // 检查是否接收完成
                        if (ws_buffer.len == data->payload_len) {
                            // 处理完整的数据
                            process_websocket_data(ws_buffer.buffer, ws_buffer.len);
                            // 重置缓冲区
                            ws_buffer.receiving = false;
                            ws_buffer.len = 0;
                        } else {
                            // ESP_LOGI(TAG, "Buffering: received %d/%d bytes", 
                            //     ws_buffer.len, data->payload_len);
                        }
                    }
                    break;
                    
                case WS_TRANSPORT_OPCODES_BINARY:
                    // 处理二进制数据
                    ESP_LOGI(TAG, "Received binary data");
                    break;
                    
                case WS_TRANSPORT_OPCODES_PING:
                    // ESP_LOGI(TAG, "Received PING");
                    break;
                    
                case WS_TRANSPORT_OPCODES_PONG:
                    // ESP_LOGI(TAG, "Received PONG");
                    break;
                    
                case WS_TRANSPORT_OPCODES_CLOSE:
                    ESP_LOGI(TAG, "Received CLOSE");
                    send_trace_log("ESP WebSocket 关闭", "");
                    // 服务端发起的关闭 重连
                    // re_connect_websocket();
                    break;
                    
                case WS_TRANSPORT_OPCODES_CONT:
                    // 处理连续帧
                    ESP_LOGI(TAG, "Received continuation frame");
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown opcode: %d", data->op_code);
                    break;
            }
            break;
    }
}

// 初始化播放缓冲区
static void init_play_buffer() {
    play_buffer.rb = rb_create(AUDIO_PLAY_BUFFER_SIZE, 1);  // 创建环形缓冲区
    play_buffer.mutex = xSemaphoreCreateMutex();
}

// 修改写入函数
static size_t play_buffer_write(const char *data, size_t len) {
    if (!play_buffer.rb || !data || len == 0) return 0;
    
    xSemaphoreTake(play_buffer.mutex, portMAX_DELAY);
    
    size_t available = rb_bytes_available(play_buffer.rb);
    if (available >= len) {
        rb_write(play_buffer.rb, data, len, portMAX_DELAY);
        play_buffer.stats.total_writes++;
        play_buffer.stats.bytes_written += len;
    } else {
        play_buffer.stats.buffer_full_count++;
        len = 0;  // 如果空间不够，返回0
    }
    
    xSemaphoreGive(play_buffer.mutex);
    return len;
}

// 修改读取函数，使用peek先查看数据
static size_t play_buffer_read(char *data, size_t max_len) {
    if (!play_buffer.rb || !data || max_len == 0) return 0;
    
    xSemaphoreTake(play_buffer.mutex, portMAX_DELAY);
    
    size_t read_len = 0;
    size_t filled = rb_bytes_filled(play_buffer.rb);
    
    if (filled >= 2) {  // 至少有长度信息
        // 读取长度信息
        uint8_t len_bytes[2];
        rb_read(play_buffer.rb, (char *)len_bytes, 2, portMAX_DELAY);
        uint16_t packet_len = (len_bytes[0] << 8) | len_bytes[1];
        // 读取数据包
        if (packet_len <= max_len - 2) {  // 确保有足够空间存储数据
            // 复制长度信息
            memcpy(data, len_bytes, 2);
            // 读取实际数据
            size_t actual_read = rb_read(play_buffer.rb, data + 2, packet_len, portMAX_DELAY);
            if (actual_read == packet_len) {
                read_len = packet_len + 2;
                play_buffer.stats.total_reads++;
                play_buffer.stats.bytes_read += read_len;
                play_buffer.stats.opus_stats.played_frames++;
            }
        } else {
        }
    }
    
    xSemaphoreGive(play_buffer.mutex);
    return read_len;
}


// 修改音频播放任务
static void audio_play_task(void *pvParameters) {
    char *play_chunk = audio_calloc(1, PLAY_CHUNK_SIZE);
    uint32_t empty_count = 0;  // 用于跟踪缓冲区空的次数
    
    while (1) {
        // 计算当前缓冲区使用率
        size_t filled = rb_bytes_filled(play_buffer.rb);
        size_t total = rb_get_size(play_buffer.rb);
        float usage = (float)filled / total * 100;

        if (!is_playing_cache) {
            // 当缓冲区数据达到阈值时开始播放
            if (usage >= get_buffer_play_threshold()) {
                set_first_audio_after_connect(false);
    
                set_is_playing_cache(true);
                // 准备开始播放 ai的音频
                audio_tone_stop();
                // audio_tone_url_stop();

                player_pipeline_run(s_volc.player_pipeline);
                user_event_notify(USER_EVENT_AI_SPEAKING);
                set_last_audio_time(esp_timer_get_time());
                set_voice_sleep_flag(false);
                empty_count = 0;
                ESP_LOGI(TAG, "[CZID:%u] Buffer reached threshold (%.1f%%), starting playback fill/total: %d/%d", get_coze_conversation_id(), usage, filled, total);
            }
        } else {
            // 当缓冲区为空时，等待一段时间后再停止播放
#if 1
            if (filled == 0) {
                empty_count++;
                // 连续多次检测到缓冲区为空时停止播放
                if (empty_count == 30) {  // 约100ms后
                    ESP_LOGI(TAG, "=== Buffer empty detected");
                }
                // if (empty_count > 30) {  // 约100ms后
                //     set_is_playing_cache(false);
                //     user_event_notify(USER_EVENT_STANDBY);
                //     lcd_state_event_send(EVENT_LISTEN);
                //     ESP_LOGI(TAG, "Buffer empty, stopping playback");
                // }
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                empty_count = 0;
            }
#endif
        }

#if 1
    if (is_playing_cache) {
        uint32_t current_time = esp_timer_get_time() / 1000;  // 转换为毫秒
        uint32_t i2s_last_playing_time = get_i2s_last_playing_time();
        if (current_time - i2s_last_playing_time > 300) {
            set_is_playing_cache(false);
            user_event_notify(USER_EVENT_STANDBY);
            if(get_valuestate() != state_VALUE0_close)
            {
                lcd_state_event_send(EVENT_LISTEN);
            }
            else
            {
                lcd_state_event_send(EVENT_OFF);
            }
            ESP_LOGW(TAG, "[CZID:%u] i2s play timeout, stopping playback", get_coze_conversation_id());
        }
    }
#endif

        if (is_playing_cache && !get_manual_break_flag() ) {
            size_t read_len = play_buffer_read(play_chunk, PLAY_CHUNK_SIZE);
            if (read_len > 0) {

                // 判断play_chunk 的格式是否正确
                uint16_t out_len = (play_chunk[0] << 8) | play_chunk[1];
                if (out_len != read_len - 2 || (read_len - 2) != OPUS_FRAME_SIZE) {
                    ESP_LOGI(TAG, "play_chunk format error, discard");
                    continue;
                }
                // 关盖 关机 就不播了
                if(get_hall_state() == HALL_STATE_ON && get_valuestate() != state_VALUE0_close)
                {
                    raw_stream_write(player_pipeline_get_raw_write(s_volc.player_pipeline), 
                                    play_chunk, read_len);
                }
                else
                {
                    if (play_buffer.rb) {
                        xSemaphoreTake(play_buffer.mutex, portMAX_DELAY);
                        rb_reset(play_buffer.rb);
                        memset(&play_buffer.stats, 0, sizeof(play_buffer.stats));
                        xSemaphoreGive(play_buffer.mutex);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(PACKAGE_MS - 5));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

    }
    
    audio_free(play_chunk);
    vTaskDelete(NULL);
}

// 修改初始化函数，创建队列和处理任务
void socket_handler_init(sdk_actived_mode_t actived_mode) {
    static bool first_init = true;
    send_trace_log("开始初始化 Web Socket", "");

    esp_log_level_set("AFE_VC", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "socket_handler_init");
    s_volc.actived_mode = actived_mode;

    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        recording_flag = true;
    } else {
        recording_flag = false;
    }

    // 如果已经初始化过，则不重复初始化
    if (!first_init) {
        ESP_LOGW(TAG, "socket_handler_init already initialized");
        return;
    }
    first_init = false;

    // 初始化音频相关组件
    s_volc.frame_q = xQueueCreate(30, sizeof(frame_package_t));
    s_volc.data_proc_running = true;
    
    // 创建音频数据队列
    audio_data_queue = xQueueCreate(AUDIO_QUEUE_LENGTH, sizeof(audio_data_t));
    if (!audio_data_queue) {
        ESP_LOGE(TAG, "Failed to create audio data queue");
        return;
    }

    // 创建语音数据队列
    voice_queue = xQueueCreate(VOICE_QUEUE_LENGTH, sizeof(voice_data_t));
    if (!voice_queue) {
        ESP_LOGE(TAG, "Failed to create voice data queue");
        return;
    }
    
    ESP_LOGI(TAG, "Initializing WebSocket client");
    // 打开音频管道
    open_audio_pipeline();
    
    // 唤醒词才用这个
    if (s_volc.actived_mode == SDK_ACTIVED_MODE_BUTTON_AND_WAKEUP || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        s_volc.recorder_engine = audio_record_engine_init(s_volc.record_pipeline, rec_engine_cb, s_volc.actived_mode);
    } else {
        s_volc.recorder_engine = NULL;
    }
    s_volc.esp_dispatcher = esp_dispatcher_get_delegate_handle();

    // 创建音频处理任务
    audio_thread_create(NULL, "voice_read_task", voice_read_task, NULL, 8 * 1024, 10, true, 1);
    audio_thread_create(NULL, "voice_send_task", voice_send_task, NULL, 16 * 1024, 10, true, 1);

    // 初始化文本缓冲区
    init_text_buffer();

    // 创建WebSocket数据队列
    ws_data_queue = xQueueCreate(WS_QUEUE_LENGTH, sizeof(ws_data_t));
    if (!ws_data_queue) {
        ESP_LOGE(TAG, "Failed to create WebSocket data queue");
        return;
    }
    
#if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    init_opus_cache();
#endif

// #ifndef CONFIG_WAKE_UP_NONE
//     micro_speech_setup(speech_recognition_result);
//     micro_speech_start();
// #endif

    // 初始化播放缓冲区
    init_play_buffer();
    
    // 创建音频播放任务
    audio_thread_create(NULL, "audio_play", audio_play_task, NULL, 4 * 1024, 10, true, 1);

    send_trace_log("初始化WebSocket 结束", "");
}

// 添加 VAD 状态跟踪结构
typedef struct {
    // vad_state_t current_state;    // 当前状态
    vad_state_t pending_state;    // 待确认状态
    int32_t state_start_time;     // 状态开始时间
    // bool state_confirmed;         // 状态是否已确认
} vad_tracker_t;

static vad_tracker_t vad_tracker = {
    // .current_state = VAD_SILENCE,
    .pending_state = VAD_SILENCE,
    .state_start_time = 0,
    // .state_confirmed = false
};


void vad_is_detect_task(void *pvParameters) {
    if (get_is_playing_cache()) {
        xTaskCreate(break_rec_with_key, "break_rec_with_key", 1024 * 4, NULL, 5, NULL);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    start_recorder_with_key();
    vTaskDelete(NULL);
}

bool g_audio_recorder_get_wakeup_state()
{
    if (s_volc.recorder_engine) {
        return audio_recorder_get_wakeup_state(s_volc.recorder_engine);
    }
    else
    {
        return 0;
    }
}


static void voice_read_task(void *pvParameters) {
    const int voice_data_read_sz = recorder_pipeline_get_default_read_size();
    int ret = 0;

    // 初始化音频缓冲区
    init_audio_ring_buffer();

    bool runing = true;
    while (runing) {

        uint8_t *voice_data = audio_calloc(1, voice_data_read_sz);
        if (s_volc.recorder_engine) {
            // if (audio_recorder_get_wakeup_state(s_volc.recorder_engine)) {
                // 唤醒词模式，如果休眠了还在读取，会有警告
                ret = audio_recorder_data_read(s_volc.recorder_engine, voice_data, voice_data_read_sz, portMAX_DELAY);
                if (voice_data_read_sz == ret) {
                    printf("#", ret);
                } else {
                    printf("#%d", ret);
                }

// #if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
// #warning "RTC_AUDIO_SAVE_TO_FLASH enabled in i2s_read_cb"
//         if (get_audio_tone_playing() || get_recording_flag()) {
//             printf("@");
//             audio_append_to_flash_buffer(voice_data, voice_data_read_sz);
//         }
// #endif
            // } else {
            //     // 唤醒词模式，如果休眠了还在读取，会有警告
            //     ret = 0;
            // }
        } else {
            ret = recorder_pipeline_read(s_volc.record_pipeline, voice_data, voice_data_read_sz);
        }


        if (voice_sleep_flag || !get_wakeup_flag()) {
            audio_free(voice_data);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        } else if (ret != voice_data_read_sz) {
            audio_free(voice_data);
            if (ret > 0) {
                printf("=%d", ret);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 将音频数据发送到队列
        voice_data_t frame = {
            .data = voice_data,
            .len = voice_data_read_sz
        };

        if (xQueueSend(voice_queue, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
            audio_free(voice_data);
            vTaskDelay(pdMS_TO_TICKS(5));
            printf("*");
            continue;
        } else {
            reset_rb_out_sec();
            vTaskDelay(pdMS_TO_TICKS(1));
            printf("+");
        }
    }

    free_audio_ring_buffer();
    vTaskDelete(NULL);
}

static void voice_send_task(void *pvParameters) {
    voice_data_t frame;
    int voice_data_read_sz = recorder_pipeline_get_default_read_size();
    uint8_t *voice_data = NULL;
    
    bool runing = true;

    while (runing) {
        // 从队列接收音频数据
        if (xQueueReceive(voice_queue, &frame, portMAX_DELAY) == pdTRUE) {

            voice_data = frame.data;
            voice_data_read_sz = frame.len;

            if ( get_socket_client() == NULL || !is_websocket_connected() || !voice_data ) 
            {
                audio_free(voice_data);
                continue;
            }
            // audio_buffer_write(frame.data, frame.len);

            if (voice_data_read_sz > 0 && wakeup_flag && !get_audio_tone_playing() && !audio_tone_url_is_playing()) {
                audio_buffer_write(voice_data, voice_data_read_sz);
                if (recording_flag) {
                    send_voice_data_to_server(voice_data, voice_data_read_sz);
                    printf("~");
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // 释放内存
            audio_free(voice_data);
        }
        // else {
        //     ESP_LOGI(TAG, "voice_send_task - xQueueReceive failed");
        //     vTaskDelay(pdMS_TO_TICKS(10));
        // }
    }
    
    vTaskDelete(NULL);
}


void leave_room(void)
{
    send_trace_log("离开房间", "");
    if (socket_client) {
        cleanup_websocket();
        socket_client = NULL;
    }
    // set_room_info_req_success(0);
    set_room_info_request_id(xTaskGetTickCount()+battery_get_voltage());
    send_trace_log("离开房间结束", "");
}


void inner_join_room()
{
    send_trace_log("加入房间 Inner", "");
    if (socket_client != NULL) {
        cleanup_websocket();
        // 等待退出
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    init_websocket();
    send_trace_log("加入房间结束 Inner", "");
    vTaskDelete(NULL);
}
void join_room(rtc_params_t* params, bool is_switch)
{
    if (!params) {
        ESP_LOGE(TAG, "Invalid parameters");
        // set_room_info_req_success(0);
        return;
    }

    // 检查必要的参数
    if (!params->bot_id[0] || !params->voice_id[0] || !params->user_id[0] || 
        !params->conv_id[0] || !params->access_token[0]) {
        ESP_LOGE(TAG, "Missing required parameters");
        // // set_room_info_req_success(0);
        return;
    }

    // 判断access_token和bot_id是否一样
    if ( get_socket_client() != NULL )
    {
        if( strcmp(ws_params.access_token, params->access_token) == 0 && strcmp(ws_params.bot_id, params->bot_id) == 0) {
            ESP_LOGI(TAG, "Access token and bot ID are the same, exiting...");
            return;
        }
        else {
            // 收到不一样
            ESP_LOGI(TAG, "Access token and bot ID are not same, will join room");
            // set_room_info_request_id(xTaskGetTickCount()+battery_get_voltage());
        }
    }

    // 保存参数
    send_trace_log("加入房间", "");
    strncpy(ws_params.bot_id, params->bot_id, sizeof(ws_params.bot_id) - 1);
    strncpy(ws_params.voice_id, params->voice_id, sizeof(ws_params.voice_id) - 1);
    strncpy(ws_params.user_id, params->user_id, sizeof(ws_params.user_id) - 1);
    strncpy(ws_params.conv_id, params->conv_id, sizeof(ws_params.conv_id) - 1);
    strncpy(ws_params.access_token, params->access_token, sizeof(ws_params.access_token) - 1);
    if(strlen(params->workflow_id) > 0) {
        strncpy(ws_params.workflow_id, params->workflow_id, sizeof(ws_params.workflow_id) - 1);
    }


    // 如果存在config，保存到ws_params
    if (params->config) {
        if (ws_params.config) {
            cJSON_Delete(ws_params.config);
        }
        ws_params.config = cJSON_Duplicate(params->config, 1);
    }

    xTaskCreatePinnedToCore(inner_join_room, "join_room", 1024 * 4, NULL, 5, NULL, 1);

    // if (is_switch) {
    //     set_manual_break_flag(true);
    //     stop_audio(true);
    //     vTaskDelay(pdMS_TO_TICKS(200));
    //     user_event_notify(USER_EVENT_CHANGING_AI_AGENT);
    // }
    send_trace_log("加入房间", "");
}


void init_websocket() {

    static int error_count = 0;
    static TickType_t last_error_time = 0;
    TickType_t current_time = xTaskGetTickCount();
    static bool first_init = true;
    
    send_trace_log("开始启动 ESP WebSocket", "");

    ws_initializing = true;

    // 第一次初始化的特殊处理
    if (first_init) {
        ws_event_group = xEventGroupCreate();
        xTaskCreate(ws_reconnect_task, "ws_reconnect", 4096, NULL, 5, NULL);
        first_init = false;
    }

    // 使用全局变量
    if(strlen(ws_params.workflow_id)) {
        snprintf(ws_uri, sizeof(ws_uri), "ws://ws.coze.cn/v1/chat?bot_id=%s&workflow_id=%s", ws_params.bot_id, ws_params.workflow_id);
    } else {
        snprintf(ws_uri, sizeof(ws_uri), "ws://ws.coze.cn/v1/chat?bot_id=%s", ws_params.bot_id);
    }
    product_info_t *pInfo = get_product_info();

    snprintf(ws_headers, sizeof(ws_headers), "Authorization: Bearer %s\r\nX-Coze-DeviceId: %s\r\n", ws_params.access_token, pInfo->szDID);

    // 如果已有客户端，先清理
    if (socket_client != NULL) {
        ESP_LOGI(TAG, "Cleaning up existing WebSocket client before initialization");
        esp_websocket_client_stop(socket_client);
        vTaskDelay(pdMS_TO_TICKS(100));  // 给一点时间完成停止
        esp_websocket_client_destroy(socket_client);
        socket_client = NULL;
        set_websocket_connected(false);
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = ws_uri,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .buffer_size = 8192 * 32,
        .task_stack = 8192,
        .task_prio = 3,
        .network_timeout_ms = 10000,
        .reconnect_timeout_ms = 1000,
        // .disable_auto_reconnect = true,
        // .pingpong_timeout_sec = 4,
        // .ping_interval_sec = 5,
        // .keep_alive_enable = true,
        // .keep_alive_idle = 12,
        // .keep_alive_interval = 4,
        // .keep_alive_count = 4,
        .headers = ws_headers
    };

    // 打印连接信息
    ESP_LOGI(TAG, "WebSocket Configuration:");
    ESP_LOGI(TAG, "URI: %s", websocket_cfg.uri);
    ESP_LOGI(TAG, "Transport: %s", websocket_cfg.transport == WEBSOCKET_TRANSPORT_OVER_SSL ? "SSL" : "TCP");
    ESP_LOGI(TAG, "Network timeout: %d ms", websocket_cfg.network_timeout_ms);

    socket_client = esp_websocket_client_init(&websocket_cfg);
    if (!socket_client) {
        send_trace_log("ESP WebSocket 初始化失败", "");
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        ws_initializing = false;
        // xSemaphoreGive(ws_init_mutex);
        vTaskDelay(pdMS_TO_TICKS(1000));
        // re_connect_websocket();  // 重试
        return;
    }

    ESP_LOGI(TAG, "Registering WebSocket events...");
    esp_err_t ret = esp_websocket_register_events(socket_client, 
                                               WEBSOCKET_EVENT_ANY,
                                               websocket_event_handler,
                                               NULL);
    if (ret != ESP_OK) {
        send_trace_log("注册 ESP WebSocket 事件失败", "");
        ESP_LOGE(TAG, "Failed to register WebSocket events: %d", ret);
        esp_websocket_client_destroy(socket_client);
        socket_client = NULL;
        ws_initializing = false;
        // xSemaphoreGive(ws_init_mutex);
        return;
    }

    ESP_LOGI(TAG, "Starting WebSocket client...");
    ret = esp_websocket_client_start(socket_client);
    if (ret != ESP_OK) {
        send_trace_log("启动 ESP WebSocket 失败", "");
        ESP_LOGE(TAG, "Failed to start WebSocket client: %d", ret);
        char err_buf[256];
        esp_err_to_name_r(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "Error name: %s", err_buf);
        esp_websocket_client_destroy(socket_client);
        socket_client = NULL;
        ws_initializing = false;
        // xSemaphoreGive(ws_init_mutex);



        // 检查时间间隔是否在30秒内
        if ((current_time - last_error_time) < (30 * 1000 / portTICK_PERIOD_MS)) {
            error_count++;
        } else {
            // 超过30秒，重置计数器
            error_count = 1;
        }

        last_error_time = current_time;

        // 如果在30秒内出现3次错误
        if (error_count >= 3) {
            ESP_LOGE(TAG, "Error occurred 3 times within 30 seconds, taking action...");
            // 执行需要的操作，例如重启设备
            vTaskDelay(pdMS_TO_TICKS(2000));  // 等待日志输出和音频播放
            audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
            vTaskDelay(pdMS_TO_TICKS(2500));  // 等待日志输出和音频播放
            esp_restart();
        }
        return;
    }

    error_count = 0;
    last_error_time = 0;
    ESP_LOGI(TAG, "WebSocket client initialized successfully");
    send_trace_log("启动 ESP WebSocket 成功", "");
    // 初始化完成
    ws_initializing = false;
    // xSemaphoreGive(ws_init_mutex);
}

void end_recorder_task() {

    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        // 云端vad 不需要 end_recorder 方法
        vTaskDelete(NULL);
        return;
    }

// 测试模式
#ifdef CONFIG_TEST_MODE_VOICE
    // 增加计数器
    increment_recorder_counter();
    
    // 检查计数器
    if (get_recorder_counter() > 3) {
        ESP_LOGE(TAG, "Too many pending recordings (%d)", get_recorder_counter());
        if (last_transcript_message) {
            report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_ERROR, 
                    "Too many pending recordings", last_transcript_message);
        } else {
            report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_ERROR, 
                    "Too many pending recordings", NULL);
        }
        
        reset_recorder_counter();  // 重置计数器
    }
#endif

    end_recorder();

    vTaskDelete(NULL);
}

void end_recorder() {
    // 如果没有在录音，直接返回
    send_trace_log("结束录音", "");

    if (recording_flag == false) {
        send_trace_log("结束录音，未开始录音，忽略结束录音", "");
        ESP_LOGI(TAG, "Not recording, ignore end_recorder");
        return;
    }
    set_recording_flag(false);

    // 创建事件 ID (使用时间戳)
    char event_id[32];
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());


    // vTaskDelay(300);
    // 延时 200ms 发送结束的数据
    // 构建完成消息
    char message[256];
    snprintf(message, sizeof(message),
        "{"
            "\"id\":\"%s\","
            "\"event_type\":\"input_audio_buffer.complete\","
            "\"data\":{}"
        "}", event_id);

    // 发送完成消息
    if (is_websocket_connected()) {
        esp_err_t ret = esp_websocket_client_send_text(socket_client, message, strlen(message), 4000);
        if (ret < 0) {
            send_trace_log("发送语音完成消息失败", "");
            ESP_LOGE(TAG, "Failed to send complete message: %d", ret);
        } else {
            send_trace_log("发送语音完成消息成功，等待 coze 处理音频数据", "");
            ESP_LOGI(TAG, "Sent audio complete message with event_id: %s", event_id);
        }
    }
    user_event_notify(USER_EVENT_STANDBY);
}

void start_recorder() {
    send_trace_log("开始录音", "");

    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        if (wakeup_flag == false) {
            ESP_LOGI(TAG, "audio_recorder_trigger_start in %s", __FUNCTION__);
            audio_record_trigger_start(s_volc.recorder_engine, s_volc.actived_mode);
            // wakeup_flag = true;
            SET_WAKEUP_FLAG(true);
        }
    }

    if (recording_flag == true) {
        send_trace_log("已经在录音了，忽略启动录音", "");
        ESP_LOGI(TAG, "Ignoring start_recorder while AI is recording");
        return;
    }

    set_recording_flag(true);

    player_pipeline_run(s_volc.player_pipeline);

    if (is_playing_cache) {
        send_trace_log("开始录音，先停止播放", "");
        ESP_LOGI(TAG, "Stopping player pipeline");
        stop_audio(true);
    }
    // 停止语音识别
    user_event_notify(USER_EVENT_USER_SPEAKING);
    

#ifdef CONFIG_TEST_MODE_VOICE
    // 清空log id
    if (last_transcript_message) {
        free(last_transcript_message);
        last_transcript_message = NULL;
    }
#endif
    
}

void break_rec_with_key() {
    ESP_LOGI(TAG, "run break");
    set_voice_sleep_flag(false);
    set_last_audio_time(esp_timer_get_time());
    stop_audio(false);
    send_trace_log("开始中断录音", "");
    send_conversation_chat_cancel("break_rec_with_key");
        // xTaskCreate(cancel_ai_agent_task, "cancel_ai_agent_task", 1024 * 4, "break_rec_with_key", 5, NULL);
    send_trace_log("中断录音结束", "");
    vTaskDelete(NULL);
}

void start_recorder_with_key() {
    send_trace_log("按键触发开始录音", "");
    // 更新休眠时间
    set_last_audio_time(esp_timer_get_time());
    // 唤醒
    set_voice_sleep_flag(false);
    
    if (s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD || s_volc.actived_mode == SDK_ACTIVED_MODE_SERVER_VAD_AND_WAKEUP) {
        // 云端vad 不需要 start_recorder 方法
        send_trace_log("唤醒词模式，运行 audio_recorder_trigger_start", "");
        ESP_LOGI(TAG, "audio_recorder_trigger_start in %s", __FUNCTION__);
        audio_record_trigger_start(s_volc.recorder_engine, s_volc.actived_mode);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "run start_recorder_with_key");

    uint8_t *buffer_data = audio_calloc(1, AUDIO_BUFFER_SIZE);  // 用于获取缓冲区数据
    size_t buffered_size = audio_buffer_get_and_clear(buffer_data);
    if (buffered_size > 0) {
        send_trace_log("存在缓冲数据，发送缓冲数据到云端", "");
        ESP_LOGI(TAG, "Sending cache voice data to server, size: %d", buffered_size);
        
        // 按160字节分割数据
        const size_t CHUNK_SIZE = 160;
        size_t num_chunks = buffered_size / CHUNK_SIZE;
        
        for (size_t i = 0; i < num_chunks; i++) {
            uint8_t *chunk = buffer_data + (i * CHUNK_SIZE);
            send_voice_data_to_server(chunk, CHUNK_SIZE);
        }
        
        ESP_LOGI(TAG, "Sent %d chunks of voice data", num_chunks);
    }
    
    start_recorder();

    audio_free(buffer_data);
    
    vTaskDelete(NULL);
}

// 清理WebSocket客户端
void cleanup_websocket(void) {
    set_websocket_connected(false);
    send_trace_log("开始清理 WebSocket", "");
    ESP_LOGI(TAG, "Cleaning up WebSocket client...");

    // 停止录音
    set_recording_flag(false);
    
    // 停止播放标志
    agent_is_pushing = false;

    // 清理播放缓冲区
    if (play_buffer.rb) {
        xSemaphoreTake(play_buffer.mutex, portMAX_DELAY);
        rb_reset(play_buffer.rb);
        memset(&play_buffer.stats, 0, sizeof(play_buffer.stats));
        xSemaphoreGive(play_buffer.mutex);
    }

    // 清理opus缓存
    #if defined (CONFIG_AUDIO_SUPPORT_OPUS_DECODER)
    if (opus_cache.buffer) {
        opus_cache.len = 0;
    }
    #endif

    // 清理WebSocket缓冲区
    if (ws_buffer.buffer) {
        ws_buffer.len = 0;
        ws_buffer.receiving = false;
    }

    // 清理文本缓冲区
    if (text_buffer.buffer) {
        text_buffer.len = 0;
    }

    // 清空WebSocket数据队列
    if (ws_data_queue) {
        ws_data_t ws_data;
        while (xQueueReceive(ws_data_queue, &ws_data, 0) == pdTRUE) {
            if (ws_data.data) {
                free(ws_data.data);
    }
        }
    }
    
    // 停止WebSocket客户端
    if (socket_client) {
        // socket_client = NULL;
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Stopping WebSocket client...");
        esp_err_t ret = esp_websocket_client_stop(socket_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop WebSocket client: %d", ret);
        }
        esp_websocket_client_destroy(socket_client);
        socket_client = NULL;
        ESP_LOGI(TAG, "WebSocket client cleanup completed");
        } else {
        ESP_LOGI(TAG, "WebSocket client already NULL");
    }
    send_trace_log("清理 WebSocket 结束", "");
}


void ws_send_timeout_check_callback(void) {

    static TickType_t first_entry_time = 0;
    static TickType_t last_entry_time = 0;

    printf("ws_sned_to ft %d lt %d\n", first_entry_time, last_entry_time);

    TickType_t current_time = xTaskGetTickCount();

    // 如果是第一次进入，记录第一次进入时间
    if (first_entry_time == 0) {
        first_entry_time = current_time;
        last_entry_time = current_time;
    }

    // 检查是否在1秒内进入
    if (current_time - last_entry_time <= pdMS_TO_TICKS(1000)) {
        // 刷新最后一次进入时间
        last_entry_time = current_time;
    } else {
        // 超过1秒未进入，重置第一次进入时间
        first_entry_time = current_time;
        last_entry_time = current_time;
    }

    // 如果5秒内每秒都进入
    if (last_entry_time - first_entry_time >= pdMS_TO_TICKS(7000)) {
        // 重置第一次进入时间
        first_entry_time = 0;
        last_entry_time = 0;
        audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
        vTaskDelay(pdMS_TO_TICKS(2500));
        cleanup_websocket();
        set_room_info_request_id(xTaskGetTickCount()+battery_get_voltage());
    }
}


void send_voice_data_to_server(uint8_t *voice_data, int len) {
    if (!is_websocket_connected() || !voice_data || len <= 0) {
        return;
    }

    // 计算 base64 编码后的长度
    size_t out_len = 4 * ((len + 2) / 3);  // base64 编码后的长度
    char *base64_buffer = malloc(out_len + 1);  // +1 for null terminator
    if (!base64_buffer) {
        ESP_LOGE(TAG, "Failed to allocate base64 buffer");
        return;
    }

#if defined (CONFIG_RTC_AUDIO_SAVE_TO_FLASH)
#warning "RTC_AUDIO_SAVE_TO_FLASH enabled in send_voice_data_to_server"
    // 保存到缓冲区
    // audio_append_to_flash_buffer(voice_data, len);
#endif

    // 进行 base64 编码
    size_t encoded_len;
    mbedtls_base64_encode((unsigned char *)base64_buffer, out_len + 1, &encoded_len, 
                         voice_data, len);
    base64_buffer[encoded_len] = '\0';

    // 创建事件 ID (使用时间戳)
    char event_id[32];
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());

    // 构建消息
    char *message = malloc(encoded_len + 256);  // 256 for JSON structure
    if (!message) {
        ESP_LOGE(TAG, "Failed to allocate message buffer");
        free(base64_buffer);
        return;
    }

    // 格式化消息
    snprintf(message, encoded_len + 256,
        "{"
            "\"id\":\"%s\","
            "\"event_type\":\"input_audio_buffer.append\","
            "\"data\":{"
                "\"delta\":\"%s\""
            "}"
        "}", event_id, base64_buffer);

    // 发送消息
    // ESP_LOGI(TAG, "Sending voice data to server %d", strlen(message));
    esp_err_t ret = esp_websocket_client_send_text(socket_client, message, strlen(message), 4000);
    if (ret < 0) {
        send_trace_log("发送语音数据失败", "");
        ESP_LOGE(TAG, "Failed to send voice data: %d", ret);

        vTaskDelay(pdMS_TO_TICKS(5));


        ws_send_timeout_check_callback();
#if 0   // todo Peter check robustness add
        static int send_fail_count = 0;
        static int64_t last_reset_time = 0;

        // 获取当前时间
        int64_t current_time = esp_timer_get_time();

        // 如果超过1分钟，重置计数器
        if (current_time - last_reset_time > 10000000) { // 60秒 = 60000000微秒
            send_fail_count = 0;
            last_reset_time = current_time;
        }

        // 增加失败计数
        send_fail_count++;

        // 如果在X分钟内失败次数达到Y次，重启系统(1秒能出现100次)
        if (send_fail_count >= 2000) {
            ESP_LOGE(TAG, "ws send fail count >= 60, restart");
            audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
            vTaskDelay(pdMS_TO_TICKS(2500));
            esp_restart();
        }
#endif
    } else {
        // ESP_LOGI(TAG, "Sent voice data: %d", ret);
        // printf(">");
    }

    // 清理
    free(base64_buffer);
    free(message);


#if defined (CONFIG_RTC_AUDIO_SAVE_TO_SERVER)
    // 保存到缓冲区
    // bufferAudioChunk(voice_data, len, AUDIO_BUFFER_TYPE_AEC);
#endif
}


#ifdef CONFIG_TEST_MODE_VOICE
static int recorder_counter = 0;
static portMUX_TYPE recorder_counter_mutex = portMUX_INITIALIZER_UNLOCKED;

void increment_recorder_counter(void) {
    int current_count;
    portENTER_CRITICAL(&recorder_counter_mutex);
    recorder_counter++;
    current_count = recorder_counter;
    portEXIT_CRITICAL(&recorder_counter_mutex);
    
    // 在临界区外打印日志
    ESP_LOGI(TAG, "Recorder counter incremented to: %d", current_count);
}

void decrement_recorder_counter(void) {
    int current_count;
    portENTER_CRITICAL(&recorder_counter_mutex);
    if (recorder_counter > 0) {
        recorder_counter--;
    }
    current_count = recorder_counter;
    portEXIT_CRITICAL(&recorder_counter_mutex);
    
    // 在临界区外打印日志
    ESP_LOGI(TAG, "Recorder counter decremented to: %d", current_count);
}

int get_recorder_counter(void) {
    int current_count;
    portENTER_CRITICAL(&recorder_counter_mutex);
    current_count = recorder_counter;
    portEXIT_CRITICAL(&recorder_counter_mutex);
    return current_count;
}

void reset_recorder_counter(void) {
    portENTER_CRITICAL(&recorder_counter_mutex);
    recorder_counter = 0;
    portEXIT_CRITICAL(&recorder_counter_mutex);
    
    // 在临界区外打印日志
    ESP_LOGI(TAG, "Recorder counter reset to 0");
}
#endif

// 添加一个包装函数来播放网络错误提示音
void play_network_error_with_debounce(void) {
    // 获取当前时间（毫秒）
    uint32_t current_time = esp_timer_get_time() / 1000;
    
    // 检查是否在10秒内已经播放过
    if (current_time - last_network_error_play_time < 10000 && last_network_error_play_time != 0) {
        ESP_LOGI(TAG, "Network error sound already played within 10s, skipping");
        return;
    }
    
    // 更新上次播放时间
    last_network_error_play_time = current_time;
    
    // 播放网络错误提示音
    stop_audio(true);
    report_error(ERROR_TYPE_AUDIO, ERROR_LEVEL_ERROR, 
                    "network_error try reconnect", NULL);
    ESP_LOGI(TAG, "Playing network error sound");
    send_trace_log("播放网络错误提示音", "");
    user_event_notify(USER_EVENT_NET_WORK_ERROR);
}



void send_heart_beat_message(long long x) {
    // 发送初始化消息
    const char *init_message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"heartbeat:%lld\""
        "}"
    "}";
    char message[1024];
    snprintf(message, sizeof(message), init_message, x);
    ESP_LOGI(TAG, "send_heart_beat_message: %s", message);
    
    esp_err_t ret = esp_websocket_client_send_text(socket_client, message, strlen(message), 4000);
    if (ret > 0) {
        ESP_LOGI(TAG, "Init message sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send init message: %d", ret);
    }
}


void send_wakeup_message(void) {
    // 发送初始化消息
    send_trace_log("发送上电消息", "");
    const char *init_message = "{"
        "\"event_type\":\"conversation.message.create\","
        "\"data\":{"
            "\"role\":\"user\","
            "\"content_type\":\"text\","
            "\"content\":\"wakeup:1\""
        "}"
    "}";
    
    esp_err_t ret = esp_websocket_client_send_text(socket_client, init_message, strlen(init_message), 4000);
    if (ret > 0) {
        ESP_LOGI(TAG, "Init message sent successfully");
        send_trace_log("发送上电消息成功", "");
    } else {
        ESP_LOGE(TAG, "Failed to send init message: %d", ret);
        send_trace_log("发送上电消息失败", "");
    }
}

// 弱网健壮性逻辑
// 一分钟出现2次发送错误，则判断为弱网，直接重启
void ws_send_error_handle(void)
{
    static TickType_t last_error_tick = 0;
    TickType_t current_tick = xTaskGetTickCount();

    if ((current_tick - last_error_tick) < (60 * 1000 / portTICK_PERIOD_MS)) {
        ESP_LOGE(TAG, "Error occurred twice within a minute, restarting...");
        if (get_valuestate() == state_VALUE0_close) {
            ESP_LOGI(TAG, "%s MagicI is close, exiting task", __func__);
            return;
        }
        audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
        vTaskDelay(pdMS_TO_TICKS(2500));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "Error occurred, logging tick.");
        last_error_tick = current_tick;
    }
}

#define OPUS_LAST_MIN_TIME 30000

void opus_error_check_callback(void) {

    static int opus_error_count = 0;

    static TickType_t first_entry_time = 0;
    static TickType_t last_entry_time = 0;

    printf("opus_err %d ft %d lt%d\n",  opus_error_count, first_entry_time, last_entry_time);

    TickType_t current_time = xTaskGetTickCount();
    audio_tone_play(1, 0, "spiffs://spiffs/connect_wifi_retry.mp3");
    vTaskDelay(pdMS_TO_TICKS(2500));

    // 如果是第一次进入，记录第一次进入时间
    if (last_entry_time == 0) {
        last_entry_time = current_time;
        opus_error_count++;
        return;
    }

    // 检查是否在10秒内 再一次进入
    if (current_time - last_entry_time <= pdMS_TO_TICKS(OPUS_LAST_MIN_TIME)) {
        // 刷新最后一次进入时间
        last_entry_time = current_time;
        opus_error_count++;
    } else {
        // 超过10秒未出现，重置第一次进入时间
        last_entry_time = current_time;
        opus_error_count = 0;
    }

    if(opus_error_count >= 3) {
        printf("error : opus_err reboot !%d ft %d lt%d\n",  opus_error_count, first_entry_time, last_entry_time);
        audio_tone_play(1, 0, "spiffs://spiffs/T3_error_feedback_96k.mp3");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

esp_err_t g_recorder_pipeline_resume()
{
    ESP_RETURN_ON_FALSE(s_volc.record_pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_reset_ringbuffer(s_volc.record_pipeline->audio_pipeline);
    audio_pipeline_reset_elements(s_volc.record_pipeline->audio_pipeline);
    audio_pipeline_resume(s_volc.record_pipeline->audio_pipeline);
    return ESP_OK;
};

esp_err_t g_recorder_pipeline_pause()
{
    ESP_RETURN_ON_FALSE(s_volc.record_pipeline != NULL, ESP_FAIL, TAG, "recorder pipeline not initialized");
    audio_pipeline_pause(s_volc.record_pipeline->audio_pipeline);
    return ESP_OK;
};