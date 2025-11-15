#include "wx433.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

#define RMT_RX_GPIO        GPIO_NUM_11
#define RMT_TX_GPIO        GPIO_NUM_9
#define RMT_CLK_HZ         (1 * 1000 * 1000)
#define RMT_MEM_BLOCKS     64
#define RMT_TIMEOUT_US     4000
#define RMT_ONE_PACK_TIMEOUT_US 2500
#define RMT_BUFFER_LEN     512

#define WS_BUFFER_LEN      6
#define WS_BUFFER_NUM      5
#define WS_MIN_DATA_POINTS 100

#define WS499_PATTERN_LENGTH    5
#define WS499_MIN_MATCH_COUNT   1
#define WS499_BUFFER_COUNT      5

#define MAX_EVENT_LISTENERS 8

typedef struct {
    wx433_event_callback_t callback;
    void *user_data;
    bool active;
} event_listener_t;

typedef enum {
    WS499_PATTERN_NONE = 0,
    WS499_PATTERN_LEG = 1,
    WS499_PATTERN_CHEST = 2,
    WS499_PATTERN_VAGINA = 3
} ws499_pattern_type_t;

typedef struct {
    const unsigned char pattern[WS499_PATTERN_LENGTH];
    ws499_pattern_type_t type;
    const char *action_message;
    int touch_pin;
} ws499_pattern_t;

static const ws499_pattern_t ws499_patterns[] = {
    { {0x54, 0x29, 0x38, 0x83, 0x0d}, WS499_PATTERN_LEG,   "用户正在摸你大腿", WTP_PIN_LEG },
    { {0x54, 0x29, 0x38, 0x83, 0x35}, WS499_PATTERN_CHEST, "用户在摸你的奶头", WTP_PIN_CHEST },
    { {0x54, 0x29, 0x38, 0x83, 0x2d}, WS499_PATTERN_VAGINA,"用户在抽插你",    WTP_PIN_CHEST },
};

static const char *TAG = "RMT_TX_RX";
static SemaphoreHandle_t rx_complete_sem = NULL;
static QueueHandle_t tx_msg_queue = NULL;
static SemaphoreHandle_t data_ready_sem = NULL;
static uint8_t weather_data_buffer[WS_BUFFER_NUM * WS_BUFFER_LEN] = {0};
static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_rmt_encoder = NULL;
static rmt_channel_handle_t rx_channel = NULL;

static event_listener_t event_listeners[MAX_EVENT_LISTENERS] = {0};
static SemaphoreHandle_t event_listeners_mutex = NULL;

// Forward declarations for internal functions used before definition
// static void send_touch_action_message_task(void *param);
static void wx433_trigger_event(const wx433_event_t *event);
static ws499_pattern_type_t ws499_analyze_packets(unsigned char *in_packets, size_t len);

static void print_hex_buffer(const char *name, const uint8_t *buffer, uint16_t length)
{
    if (name) {
        printf("%s(%d):", name, length);
    }
    for (size_t i = 0; i < length; i++) {
        printf("%02x ", buffer[i]);
    }
    printf("\n");
}

static inline void set_bit_in_buffer(uint8_t *buffer, unsigned int bit_position, uint8_t value)
{
    if (value) {
        buffer[bit_position / 8] |= (1 << (bit_position % 8));
    } else {
        buffer[bit_position / 8] &= ~(1 << (bit_position % 8));
    }
}

static bool rmt_rx_complete_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(rx_complete_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t rmt_rx_init(void)
{
    ESP_LOGI(TAG, "RX init: gpio=%d clk=%u mem=%u timeout_us=%u", (int)RMT_RX_GPIO, (unsigned)RMT_CLK_HZ, (unsigned)RMT_MEM_BLOCKS, (unsigned)RMT_TIMEOUT_US);
    rx_complete_sem = xSemaphoreCreateBinary();
    data_ready_sem = xSemaphoreCreateBinary();

    rmt_rx_channel_config_t rx_config = {
        .gpio_num = RMT_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_CLK_HZ,
        .mem_block_symbols = RMT_MEM_BLOCKS,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rx_channel));

    rmt_rx_event_callbacks_t callbacks = { .on_recv_done = rmt_rx_complete_callback };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_LOGI(TAG, "RX channel enabled");
    return ESP_OK;
}

static void rmt_receive_task(void *priv)
{
    bool isPrintfLog = (bool)priv;
    rmt_rx_init();
    ESP_LOGI(TAG, "RX task started");

    rmt_receive_config_t receive_config = {
        .signal_range_max_ns = RMT_TIMEOUT_US * 1000ULL,
    };

    while (1) {
        rmt_symbol_word_t *rmt_buffer = (rmt_symbol_word_t *)malloc(RMT_BUFFER_LEN * sizeof(rmt_symbol_word_t));
        if (rmt_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes", (unsigned)(RMT_BUFFER_LEN * sizeof(rmt_symbol_word_t)));
            vTaskDelete(NULL);
            return;
        }

        memset(rmt_buffer, 0, RMT_BUFFER_LEN * sizeof(rmt_symbol_word_t));
        ESP_ERROR_CHECK(rmt_receive(rx_channel, rmt_buffer, RMT_BUFFER_LEN * sizeof(rmt_symbol_word_t), &receive_config));
        ESP_LOGD(TAG, "rmt_receive posted buffer=%p size=%u", (void*)rmt_buffer, (unsigned)(RMT_BUFFER_LEN * sizeof(rmt_symbol_word_t)));
        xSemaphoreTake(rx_complete_sem, portMAX_DELAY);
        ESP_LOGD(TAG, "RX done");

        uint8_t temp_buffer[WS_BUFFER_NUM][WS_BUFFER_LEN] = {0};
        int data_count = 0, bit_index = 0, buffer_index = 0;

        for (int i = 0; i < RMT_BUFFER_LEN; i++, data_count++) {
            rmt_symbol_word_t symbol = rmt_buffer[i];
            if (symbol.duration0 == 0 && symbol.duration1 == 0) break;

            if (symbol.level0 && !symbol.level1) {
                if (symbol.duration1 > RMT_ONE_PACK_TIMEOUT_US) {
                    bit_index = 0;
                    buffer_index++;
                    ESP_LOGD(TAG, "packet boundary -> buffer_index=%d", buffer_index);
                }
                set_bit_in_buffer(temp_buffer[buffer_index % WS_BUFFER_NUM], bit_index, symbol.duration0 > symbol.duration1);
                bit_index++;
            }
        }
        ESP_LOGD(TAG, "RX parsed data_count=%d bit_index=%d buffer_index=%d", data_count, bit_index, buffer_index);

        if (data_count > WS_MIN_DATA_POINTS) {
            for (int j = 0; isPrintfLog && (j < WS_BUFFER_NUM); j++) {
                print_hex_buffer("WS_DATA", temp_buffer[j], WS_BUFFER_LEN);
            }
            memcpy(weather_data_buffer, temp_buffer, sizeof(temp_buffer));
            // Analyze inside RX task to trigger events
            ws499_analyze_packets(weather_data_buffer, WS_BUFFER_NUM * WS_BUFFER_LEN);
            xSemaphoreGive(data_ready_sem);
        } else {
            ESP_LOGD(TAG, "RX ignored: data_count=%d < min=%d", data_count, WS_MIN_DATA_POINTS);
        }
        // if (isPrintfLog) {
        //     printf("rmt_receive_task task Min heap:%d \r\n", uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()));
        // }
        free(rmt_buffer);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t rmt_tx_init(void)
{
    ESP_LOGI(TAG, "TX init: gpio=%d clk=%u mem=%u", (int)RMT_TX_GPIO, (unsigned)RMT_CLK_HZ, (unsigned)RMT_MEM_BLOCKS);
    tx_msg_queue = xQueueCreate(TX_MAX * 20, sizeof(WX433_TX_EVENT_t));

    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = RMT_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_CLK_HZ,
        .mem_block_symbols = RMT_MEM_BLOCKS,
        .trans_queue_depth = 4,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_rmt_chan));
    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));
    ESP_LOGI(TAG, "TX channel enabled");
    return ESP_OK;
}

static esp_err_t rmt_tx_send_waveform(const rmt_symbol_word_t *symbols, size_t symbol_count)
{
    if (!s_rmt_chan || !symbols || symbol_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    rmt_transmit_config_t tx_config = { .loop_count = 0, .flags = { .eot_level = 0 } };
    rmt_copy_encoder_config_t encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &s_rmt_encoder));
    return rmt_transmit(s_rmt_chan, s_rmt_encoder, symbols, symbol_count * sizeof(rmt_symbol_word_t), &tx_config);
}

static esp_err_t generate_custom_waveform(uint8_t sequence[], size_t sequence_len, size_t pulse_count)
{
    rmt_symbol_word_t *symbols = (rmt_symbol_word_t *)malloc(pulse_count * sequence_len * sizeof(rmt_symbol_word_t));
    if (!symbols) {
        return ESP_ERR_NO_MEM;
    }
    memset(symbols, 0, pulse_count * sequence_len * sizeof(rmt_symbol_word_t));

    for (size_t c = 0; c < pulse_count; c++) {
        for (size_t i = 0; i < sequence_len; i++) {
            rmt_symbol_word_t *symbol = &symbols[i + sequence_len * c];
            symbol->level0 = 1;
            symbol->level1 = 0;
            symbol->duration1 = sequence[i] == '0' ? 1200 : 400;
            symbol->duration0 = sequence[i] == '0' ? 400 : 1200;
        }
        symbols[sequence_len - 1 + sequence_len * c].duration1 = 12000;
    }
    esp_err_t ret = rmt_tx_send_waveform(symbols, sequence_len * pulse_count);
    vTaskDelay(pdMS_TO_TICKS(100));
    free(symbols);
    return ret;
}

static void wx433_trigger_event(const wx433_event_t *event)
{
    if (!event) return;
    if (xSemaphoreTake(event_listeners_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_EVENT_LISTENERS; i++) {
            if (event_listeners[i].active && event_listeners[i].callback) {
                event_listeners[i].callback(event, event_listeners[i].user_data);
            }
        }
        xSemaphoreGive(event_listeners_mutex);
    }
}

static ws499_pattern_type_t ws499_identify_pattern(unsigned char *data)
{
    if (!data) return WS499_PATTERN_NONE;
    for (size_t i = 0; i < sizeof(ws499_patterns) / sizeof(ws499_patterns[0]); i++) {
        if (memcmp(data, ws499_patterns[i].pattern, WS499_PATTERN_LENGTH) == 0) {
            ESP_LOGI(TAG, "Pattern matched idx=%u type=%d", (unsigned)i, (int)ws499_patterns[i].type);
            return ws499_patterns[i].type;
        }
    }
    return WS499_PATTERN_NONE;
}

// static void send_touch_action_message_task(void *param) {
//     const char *message = (const char *)param;
//     vTaskDelete(NULL);
// }

static ws499_pattern_type_t ws499_analyze_packets(unsigned char *in_packets, size_t len)
{
    if (!in_packets || !len) {
        ESP_LOGE(TAG, "Invalid data packets pointer");
        return WS499_PATTERN_NONE;
    }

    uint8_t data_packets[WS_BUFFER_NUM][WS_BUFFER_LEN] = {0};
    memcpy(data_packets, in_packets, len);

    unsigned char pattern_occurrences[4] = {0};
    for (unsigned char i = 0; i < WS499_BUFFER_COUNT; i++) {
        ws499_pattern_type_t pattern_type = ws499_identify_pattern(data_packets[i]);
        pattern_occurrences[pattern_type]++;
    }
    ESP_LOGD(TAG, "occurrences: NONE=%u LEG=%u CHEST=%u VAGINA=%u", pattern_occurrences[0], pattern_occurrences[1], pattern_occurrences[2], pattern_occurrences[3]);

    for (size_t i = 0; i < sizeof(ws499_patterns) / sizeof(ws499_patterns[0]); i++) {
        if (pattern_occurrences[ws499_patterns[i].type] >= WS499_MIN_MATCH_COUNT) {
            printf("Detected %d pattern\n", ws499_patterns[i].type);
            // xTaskCreate(send_touch_action_message_task, "send_touch_action_message_task", 2048 * 2, (void*)ws499_patterns[i].action_message, 5, NULL);

            wx433_event_t event;
            memset(&event, 0, sizeof(event));
            event.type = (ws499_patterns[i].type == WS499_PATTERN_LEG) ? WX433_EVENT_LEG_TOUCH : WX433_EVENT_CHEST_TOUCH;
            event.data.touch.message = ws499_patterns[i].action_message;
            event.data.touch.touch_pin = ws499_patterns[i].touch_pin;
            ESP_LOGI(TAG, "Trigger event type=%d msg=%s pin=%d", (int)event.type, event.data.touch.message, event.data.touch.touch_pin);
            wx433_trigger_event(&event);
            // wtp_on_touch(ws499_patterns[i].touch_pin);
            return ws499_patterns[i].type;
        }
    }

    // const char *default_message = "用户正在摸你";
    // xTaskCreate(send_touch_action_message_task, "send_touch_action_message_task", 2048 * 2, (void*)default_message, 5, NULL);
    return WS499_PATTERN_NONE;
}

static void rmt_send_task(void *priv)
{
    bool isPrintfLog = (bool)priv;
    rmt_tx_init();
    ESP_LOGI(TAG, "TX task started");

    static const char muteSequence[]             = "0001111100001000000001100";
    static const char clipMinusSequence[]        = "0001111100001000000000100";
    static const char clipPlusSequence[]         = "0001111100001000000001000";
    static const char modeSequence[]             = "0001111100001000000000110";
    static const char volumeMinusSequence[]      = "0001111100001000000001100";
    static const char volumePlusSequence[]       = "0001111100001000000001110";
    static const char vibrationMinusSequence[]   = "0001111100001000000000010";
    static const char vibrationPlusSequence[]    = "0001111100001000000001010";

    while (1) {
        WX433_TX_EVENT_t new_element;
        if (tx_msg_queue && xQueueReceive(tx_msg_queue, &new_element, portMAX_DELAY) == pdTRUE) {
            static const char *sequences[8] = {
                muteSequence, clipMinusSequence, clipPlusSequence, modeSequence,
                volumeMinusSequence, volumePlusSequence, vibrationMinusSequence, vibrationPlusSequence
            };
            size_t sequence_lengths[8];
            for (int i = 0; i < 8; i++) sequence_lengths[i] = strlen(sequences[i]);
            ESP_LOGW(TAG, "Received new element: %d\n", new_element);
            if (new_element >= MUTE && new_element <= VIBRATION_PLUS) {
                uint8_t *data = (uint8_t *)sequences[new_element];
                size_t len = sequence_lengths[new_element];
                ESP_LOGI(TAG, "TX send event=%d len=%u", (int)new_element, (unsigned)len);
                generate_custom_waveform(data, len, new_element == MUTE ? 50 : 5);
            }
        }
        if (isPrintfLog) {
            printf("rmt_send_task task Min heap:%d \r\n", uxTaskGetStackHighWaterMark(xTaskGetCurrentTaskHandle()));
        }
    }
}

int32_t get_weather_data(uint8_t *output_buffer, size_t *output_length, bool isPrintfLog)
{
    if (!data_ready_sem || xSemaphoreTake(data_ready_sem, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "get_weather_data timeout or sem not ready");
        return -1;
    }
    if (isPrintfLog) print_hex_buffer("WS_DATA", weather_data_buffer, sizeof(weather_data_buffer));
    memcpy(output_buffer, weather_data_buffer, sizeof(weather_data_buffer));
    *output_length = sizeof(weather_data_buffer);
    ESP_LOGI(TAG, "get_weather_data len=%u", (unsigned)*output_length);
    return 0;
}

int32_t set_weather_event(WX433_TX_EVENT_t event)
{
    if (tx_msg_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "set_weather_event: %d\n", event);
    wx433_event_t tx_event;
    memset(&tx_event, 0, sizeof(tx_event));
    tx_event.type = WX433_EVENT_TX;
    tx_event.data.tx.event = event;
    wx433_trigger_event(&tx_event);
    BaseType_t ok = xQueueSend(tx_msg_queue, &event, pdMS_TO_TICKS(10));
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "TX queue send failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TX queued event=%d", (int)event);
    return ESP_OK;
}

esp_err_t wx433_register_event_listener(wx433_event_callback_t callback, void *user_data)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(event_listeners_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Register event listener %p", (void*)callback);
        for (int i = 0; i < MAX_EVENT_LISTENERS; i++) {
            if (event_listeners[i].active && event_listeners[i].callback == callback) {
                xSemaphoreGive(event_listeners_mutex);
                return ESP_ERR_INVALID_STATE;
            }
        }
        for (int i = 0; i < MAX_EVENT_LISTENERS; i++) {
            if (!event_listeners[i].active) {
                event_listeners[i].callback = callback;
                event_listeners[i].user_data = user_data;
                event_listeners[i].active = true;
                xSemaphoreGive(event_listeners_mutex);
                ESP_LOGI(TAG, "Registered at slot %d", i);
                return ESP_OK;
            }
        }
        xSemaphoreGive(event_listeners_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wx433_unregister_event_listener(wx433_event_callback_t callback)
{
    if (!callback || !event_listeners_mutex) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(event_listeners_mutex, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Unregister event listener %p", (void*)callback);
        for (int i = 0; i < MAX_EVENT_LISTENERS; i++) {
            if (event_listeners[i].active && event_listeners[i].callback == callback) {
                event_listeners[i].active = false;
                event_listeners[i].callback = NULL;
                event_listeners[i].user_data = NULL;
                xSemaphoreGive(event_listeners_mutex);
                ESP_LOGI(TAG, "Unregistered from slot %d", i);
                return ESP_OK;
            }
        }
        xSemaphoreGive(event_listeners_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_ERR_TIMEOUT;
}

void wx433_init(void)
{
    ESP_LOGI(TAG, "wx433_init");
    if (!event_listeners_mutex) {
        event_listeners_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "wx433_init: RX_GPIO=%d TX_GPIO=%d", (int)RMT_RX_GPIO, (int)RMT_TX_GPIO);

    // Allocate stacks in PSRAM
    StackType_t *wx433_rcv_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wx433_rcv_task_stack) {
        ESP_LOGE(TAG, "Failed to allocate rcv receive task stack in PSRAM");
        return;
    }
    StackType_t *wx433_send_task_stack = (StackType_t *)heap_caps_malloc(4 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wx433_send_task_stack) {
        heap_caps_free(wx433_rcv_task_stack);
        ESP_LOGE(TAG, "Failed to allocate send receive task stack in PSRAM");
        return;
    }

    StaticTask_t *wx433_rcv_task_buffer = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StaticTask_t *wx433_send_task_buffer = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Enable verbose RX printing and move RX to core 1 for isolation
    xTaskCreateStaticPinnedToCore(&rmt_receive_task, "wx433_rx_client", 4 * 1024, (void *)1, 5, wx433_rcv_task_stack, wx433_rcv_task_buffer, 1);
    xTaskCreateStaticPinnedToCore(&rmt_send_task, "wx433_send_client", 4 * 1024, (void *)1, 5, wx433_send_task_stack, wx433_send_task_buffer, 0);
    ESP_LOGI(TAG, "wx433 tasks created");
}


