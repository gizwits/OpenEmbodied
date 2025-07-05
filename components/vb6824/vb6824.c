#include "vb6824.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "vb_ota.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "freertos/ringbuf.h"
#include <esp_timer.h>

static const char *TAG = "vb6824";

#define UART_NUM                    CONFIG_VB6824_UART_PORT

#if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
#define AUDIO_RECV_CHENK_LEN     40
#define AUDIO_SEND_CHENK_LEN     40
#define AUDIO_SEND_CHENK_MS      20
#define SEND_BUF_LENGTH          AUDIO_SEND_CHENK_LEN*10
#define RECV_BUF_LENGTH          AUDIO_RECV_CHENK_LEN*10
#elif defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K)
#define AUDIO_RECV_CHENK_LEN     40
#define AUDIO_SEND_CHENK_LEN     320
#define AUDIO_SEND_CHENK_MS      10
#define SEND_BUF_LENGTH          1920*3
#define RECV_BUF_LENGTH          AUDIO_RECV_CHENK_LEN*10
#else
#define AUDIO_RECV_CHENK_LEN     512
#define AUDIO_SEND_CHENK_LEN     320
#define AUDIO_SEND_CHENK_MS      10
#define SEND_BUF_LENGTH          1920*3
#define RECV_BUF_LENGTH          1920*3
#endif

#define UART_QUEUE_SIZE         16
#define UART_RX_BUFFER_SIZE     AUDIO_SEND_CHENK_LEN*10
#define UART_TX_BUFFER_SIZE     AUDIO_SEND_CHENK_LEN*10

#define FRAME_MIN_LIN     (7)
#define FRAME_MAX_LIN     (512+7)

typedef struct{
    uint16_t head;
    uint16_t len;
    uint16_t cmd;
}__attribute__ ((packed))frame_head_t;

typedef struct{
    uint8_t sum;
}__attribute__ ((packed))frame_last_t;

#define SWAP_16(x)          ((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF))
#define SUM8(bytes, size)   (__sum_bytes((uint8_t*)bytes, (uint16_t)size)%256)

#define FRAME_HEAD          (SWAP_16(0x55AA))
#define FRAME_DATA_LEN(h)   (SWAP_16(h->len))
#define FRAME_CHECK(h, l)   (SUM8(h, sizeof(frame_head_t) + SWAP_16(h->len)) == l->sum)

typedef struct{
    uint16_t head;
    uint16_t len;
    uint16_t cmd;
    uint8_t data[0];
}__attribute__ ((packed))vb6824_frame_t;

typedef enum
{
    VB6824_CMD_RECV_PCM = 0x2080,
    VB6824_CMD_RECV_CTL = 0x0180,
    VB6824_CMD_RECV_WAKEUP_WORD = 0x0280,
    VB6824_CMD_RECV_OTA = 0x0105,
    VB6824_CMD_SEND_PCM = 0x2081,
    VB6824_CMD_SEND_PCM_EOF = 0x0201,
    VB6824_CMD_SEND_CTL = 0x0202,
    VB6824_CMD_SEND_VOLUM = 0x0203,
    VB6824_CMD_SEND_OTA = 0x0205,
    VB6824_CMD_SEND_GET_WAKEUP_WORD = 0x0207,
}vb6824_cmd_t;
typedef enum
{
    VB6824_MODE_AUDIO = 0,
    VB6824_MODE_OTA   = 1,
}vb6824_mode_t;

static QueueHandle_t g_uart_queue = NULL;
static RingbufHandle_t g_rx_ringbuffer = NULL;
static RingbufHandle_t g_tx_ringbuffer = NULL;
static vb6824_mode_t s_mode = VB6824_MODE_AUDIO;

static SemaphoreHandle_t g_rx_mux = NULL;

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
#include "vb_ota.h"
static jl_ota_event_t s_ota_evt = NULL;
static esp_timer_handle_t start_ota_timer = NULL;
static esp_timer_handle_t check_wakeword = NULL;
#endif
static uint8_t s_wait_fresh_wakeup_word = 1;
static uint8_t s_wait_vb_hello = 1;
static char s_wakeup_word[32] = {"你好小智"};

static void *g_voice_command_cb_arg = NULL;
static void *g_voice_event_cb_arg = NULL;
static vb_voice_command_cb_t g_voice_command_cb = NULL;
static vb_voice_event_cb_t g_voice_event_cb = NULL;

static bool g_input_enabled = false;
static bool g_output_enabled = false;

void __vb6824_frame_cb(uint8_t *frame, uint16_t frame_len);

static inline int __sum_bytes(const uint8_t* bytes, uint16_t size) {
    int sum = 0;
    for (int i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum;
}

void __frame_parse_data(uint8_t *data, uint16_t len){
    uint16_t parse_len = 0;
    uint8_t *parse_data = NULL;

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    if (s_mode == VB6824_MODE_OTA)
    {
        jl_ondata(data, len);
        // return;
    }
#endif

    static uint16_t tmp_len = {0};
    static uint8_t tmp_data[(FRAME_MAX_LIN) * 2] = {0};

    if(tmp_len == 0 && parse_len >= FRAME_MIN_LIN){
        parse_data = data;
        parse_len = len;
    }else{
        memcpy(tmp_data + tmp_len, data, len);
        tmp_len = tmp_len + len;
        parse_data = tmp_data;
        parse_len = tmp_len;
    }

re_parse:      
    if(parse_len < FRAME_MIN_LIN){
        return;
    }

    for (size_t i = 0; i < parse_len; i++){
        uint16_t left_len = parse_len - i;
        if(left_len >= FRAME_MIN_LIN){
            frame_head_t *head = (frame_head_t *)&parse_data[i];
            if(head->head == FRAME_HEAD){
                frame_last_t *last = (frame_last_t *)&parse_data[i + sizeof(frame_head_t) + FRAME_DATA_LEN(head)];
                uint16_t fr_len = sizeof(frame_head_t) + FRAME_DATA_LEN(head) + sizeof(frame_last_t);
                if(left_len >= fr_len){
                    uint16_t sum = 0;
                    uint8_t *p = (uint8_t *)head;
                    for (size_t j = 0; j < sizeof(frame_head_t) + FRAME_DATA_LEN(head); j++){
                        sum += p[j];
                    }
                    if(FRAME_CHECK(head, last)){
                        __vb6824_frame_cb(&parse_data[i], fr_len);
                        memcpy(tmp_data, &parse_data[i + fr_len], left_len - fr_len);
                        tmp_len = left_len-fr_len;
                        if(tmp_len > FRAME_MIN_LIN){
                            parse_data = tmp_data;
                            parse_len = tmp_len;
                            goto re_parse;
                        }else{
                            return;
                        }
                    }
                }else if(fr_len <= FRAME_MAX_LIN){
                    if(i != 0){
                        memcpy(tmp_data, &parse_data[i], left_len);
                        tmp_len = left_len;
                    }
                    break;
                }
            }
        }else{
            memcpy(tmp_data, &parse_data[i], left_len);
            tmp_len = left_len;
        }
    }
}

void __frame_send(vb6824_cmd_t cmd, uint8_t *data, uint16_t len){
    uint16_t packet_len = 0;
    uint8_t packet[AUDIO_SEND_CHENK_LEN + 7] = {0};

    int16_t idx = 0;
    uint16_t send_len = len;

    while (idx < len)
    {
        memset(packet, 0, sizeof(packet));
        vb6824_frame_t *frame = (vb6824_frame_t *)packet;
        frame->head = FRAME_HEAD;
        frame->len = SWAP_16(send_len);
        frame->cmd = SWAP_16(cmd);
        
        memcpy(frame->data, data + idx, (send_len>(len-idx))?(len-idx):send_len);
        idx += send_len;
        packet_len = 6 + send_len + 1;
        uint8_t checksum = 0;
        for (size_t i = 0; i < packet_len - 1; i++) {
            checksum += packet[i];
        }
        packet[packet_len - 1] = checksum;
        // ESP_LOGW(TAG, "write_bytes: %d", packet_len);
        uart_write_bytes(UART_NUM, packet, packet_len);
    }
}

void __uart_task(void *arg) {
    uart_event_t event;
    uint8_t temp_buf[1024];  // 中间缓冲
    while (true) {
        // 等待队列事件
        if (xQueueReceive(g_uart_queue, &event, pdMS_TO_TICKS(10))) {
            switch (event.type) {
                case UART_DATA:{
                        // 尝试多次读取，直到本次事件里可读字节消耗完为止
                        int len = uart_read_bytes(UART_NUM, temp_buf, sizeof(temp_buf), 0);
                        if (len > 0) {
                            __frame_parse_data(temp_buf, len);
                        }
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "HW FIFO overflow, flushing UART.");
                    uart_flush_input(UART_NUM);
                    xQueueReset(g_uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "Ring buffer full, flushing UART.");
                    uart_flush_input(UART_NUM);
                    xQueueReset(g_uart_queue);
                    break;
                default:
                    // ESP_LOGI(TAG, "UART event: %d", event.type);
                    break;
            }
        }
    }
    vTaskDelete(NULL);
}

void __uart_init(gpio_num_t tx, gpio_num_t rx){
    const uart_config_t uart_config = {
        .baud_rate = 2000000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        // .source_clk = UART_SCLK_DEFAULT,
    };

    int intr_alloc_flags = 0;
#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif
    uart_driver_install(UART_NUM, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, UART_QUEUE_SIZE, &g_uart_queue, intr_alloc_flags);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(__uart_task, "__uart_task", CONFIG_VB6824_UART_TASK_STACK_SIZE, NULL, 9, NULL);
}

#ifdef CONFIG_VB6824_SEND_USE_TASK
void __send_task(void *arg) {
    TickType_t last_time = xTaskGetTickCount();
    while (1)
    {
        if(g_output_enabled){
            size_t item_size = 0;
#if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
            uint8_t *item = (uint8_t *)xRingbufferReceive(g_tx_ringbuffer, &item_size, portMAX_DELAY);
#else
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_tx_ringbuffer, &item_size, portMAX_DELAY, AUDIO_SEND_CHENK_LEN);
#endif
            if (item != NULL) {
                if(g_output_enabled == false){
                    vRingbufferReturnItem(g_tx_ringbuffer, (void *)item);
                    goto clear_rbuffer;
                }
                TickType_t now_time = xTaskGetTickCount();
                if((now_time - last_time) >= pdMS_TO_TICKS(AUDIO_SEND_CHENK_MS)){
                    last_time = xTaskGetTickCount();
                }
                if (s_mode == VB6824_MODE_AUDIO)
                {
                    __frame_send(VB6824_CMD_SEND_PCM, (uint8_t *)item, item_size);
                }
                vRingbufferReturnItem(g_tx_ringbuffer, (void *)item);
                vTaskDelayUntil(&last_time, pdMS_TO_TICKS(AUDIO_SEND_CHENK_MS));
clear_rbuffer:
                if(g_output_enabled == false){
                    while(1){
                        size_t item_size = 0;
            #if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
                        uint8_t *item = (uint8_t *)xRingbufferReceive(g_tx_ringbuffer, &item_size, 0);
            #else
                        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_tx_ringbuffer, &item_size, 0, AUDIO_SEND_CHENK_LEN);
            #endif
                        if (item != NULL) {
                            vRingbufferReturnItem(g_tx_ringbuffer, (void *)item);
                        }else{
                            break;
                        }
                    }
                    continue;
                }
            }
        }else{
            vTaskDelay(10);
        }
    }
}
#else
void __send_timer_cb(void* arg){
    size_t item_size = 0;
#if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
    uint8_t *item = (uint8_t *)xRingbufferReceive(g_tx_ringbuffer, &item_size, 0);
#else
    uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_tx_ringbuffer, &item_size, 0, AUDIO_SEND_CHENK_LEN);
#endif
    if (item != NULL) {
        __frame_send(VB6824_CMD_SEND_PCM, (uint8_t *)item, item_size);
        vRingbufferReturnItem(g_tx_ringbuffer, (void *)item);
    }
}
#endif

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
static void vb_ota_evt_cb(jl_ota_evt_id evt, uint32_t data){
    switch (evt)
    {
    case JL_OTA_START:
        s_wait_fresh_wakeup_word = 1;
        s_mode = VB6824_MODE_OTA;
        break;
    case JL_OTA_STOP:
        s_mode = VB6824_MODE_AUDIO;    
        break;
    case JL_OTA_PROCESS:
        break;
    case JL_OTA_FAIL:
        s_mode = VB6824_MODE_AUDIO;    
    case JL_OTA_SUCCESS:
        s_mode = VB6824_MODE_AUDIO; 
        do
        {
            __frame_send(VB6824_CMD_SEND_GET_WAKEUP_WORD, (uint8_t *)&s_mode, 1);
            // ESP_LOGW(TAG, "WAIT FRESH");
            vTaskDelay(100/portTICK_PERIOD_MS);
        } while (s_wait_fresh_wakeup_word);
        data = (uint32_t)s_wakeup_word;
        break;
    default:
        break;
    }
    if (s_ota_evt)
    {
        s_ota_evt(evt, data);
    }
    
    vTaskDelay(100/portTICK_PERIOD_MS);
    if (evt == JL_OTA_SUCCESS)
    {    
        jl_ws_stop();
        esp_restart();
    }
    
}
#endif

void __vb6824_frame_cb(uint8_t *data, uint16_t len){
    vb6824_frame_t *frame = (vb6824_frame_t *)data;
    frame->len = SWAP_16(frame->len);
    frame->cmd = SWAP_16(frame->cmd);
    frame->data[frame->len] = 0;

    switch (frame->cmd)
    {
    case VB6824_CMD_RECV_PCM:{
        if(s_wait_vb_hello){
            s_wait_vb_hello = 0;
        }
        if(g_input_enabled){
            xSemaphoreTake(g_rx_mux, portMAX_DELAY);
            while(xRingbufferGetCurFreeSize(g_rx_ringbuffer) < frame->len){
                size_t item_size = 0;
#if (defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS) || defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K))
                uint8_t *item = (uint8_t *)xRingbufferReceive(g_rx_ringbuffer, &item_size, 0);
#else
                uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_rx_ringbuffer, &item_size, 0, size);
#endif
                if (item != NULL) {
                    vRingbufferReturnItem(g_rx_ringbuffer, (void *)item);
                }else{
                    break;
                }
            }
            xSemaphoreGive(g_rx_mux);
            xRingbufferSend(g_rx_ringbuffer, (void *)frame->data, frame->len, portMAX_DELAY);
        }
        break;
    } 
    case VB6824_CMD_RECV_CTL:{
        ESP_LOGI(TAG, "vb6824 recv cmd: %04x, len: %d :%.*s", frame->cmd, frame->len, frame->len, frame->data);
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
        if (strcmp((char *)frame->data, "升级模式") == 0)
        {
            if (g_voice_event_cb)
            {
                g_voice_event_cb(VB6824_EVT_OTA_ENTER, 0, g_voice_event_cb_arg);
            }
            break;
        }
        if (jl_ws_is_start()==1)
        {
            break;
        }
#endif
        if(g_voice_command_cb){
            g_voice_command_cb((char *)frame->data, frame->len, g_voice_command_cb_arg);
        }
        break;
    }
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    case VB6824_CMD_RECV_OTA:
        s_mode = VB6824_MODE_OTA;
        if (g_voice_event_cb)
        {
            g_voice_event_cb(VB6824_EVT_OTA_START, 0, g_voice_event_cb_arg);
        }
        jl_ota_start(vb_ota_evt_cb);
        break;
#endif
    case VB6824_CMD_RECV_WAKEUP_WORD:
        s_wait_fresh_wakeup_word = 0;    
        ESP_LOGI(TAG, "VB6824_CMD_SEND_GET_WAKEUP_WORD: %04x, len: %d :%.*s", frame->cmd, frame->len, frame->len, frame->data);
        memset(s_wakeup_word, 0, sizeof(s_wakeup_word));
        strncpy(s_wakeup_word, (char*)frame->data, frame->len);
        break;
    
    default:
        break;
    }
}

char *vb6824_get_wakeup_word(){
    return s_wakeup_word;
}

void vb6824_audio_enable_input(bool enable){
    if (enable == g_input_enabled) {
        return;
    }

    g_input_enabled = enable;

    if(g_input_enabled == false){
        while(1){
            size_t item_size = 0;
#if (defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS) || defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K))
            uint8_t *item = (uint8_t *)xRingbufferReceive(g_rx_ringbuffer, &item_size, 0);
#else
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_rx_ringbuffer, &item_size, 0, size);
#endif
            if (item != NULL) {
                vRingbufferReturnItem(g_rx_ringbuffer, (void *)item);
            }else{
                break;
            }
        }
    }

}

void vb6824_audio_enable_output(bool enable){
    if (enable == g_output_enabled) {
        return;
    }

    g_output_enabled = enable;

//     if(g_output_enabled == false){
//         while(1){
//             size_t item_size = 0;
// #if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
//             uint8_t *item = (uint8_t *)xRingbufferReceive(g_tx_ringbuffer, &item_size, 0);
// #else
//             uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(g_tx_ringbuffer, &item_size, 0, AUDIO_SEND_CHENK_LEN);
// #endif
//             if (item != NULL) {
//                 vRingbufferReturnItem(g_tx_ringbuffer, (void *)item);
//             }else{
//                 break;
//             }
//         }
//     }
}

void vb6824_register_voice_command_cb(vb_voice_command_cb_t cb, void *arg){
    g_voice_command_cb = cb;
    g_voice_command_cb_arg = arg;
}

void vb6824_register_event_cb(vb_voice_event_cb_t cb, void *arg){
    g_voice_event_cb_arg = arg;
    g_voice_event_cb = cb;
}

void vb6824_audio_set_output_volume(uint8_t volume){
    uint8_t vol = (uint8_t)((int)(volume * 31) / 100);
    __frame_send(VB6824_CMD_SEND_VOLUM, &vol, 1);
}

void vb6824_audio_write(uint8_t *data, uint16_t len){
    xRingbufferSend(g_tx_ringbuffer, (void *)data, len, portMAX_DELAY);
}

uint16_t vb6824_audio_read(uint8_t *data, uint16_t size){
    size_t item_size = 0;
    size_t items_waiting = 0;
    vRingbufferGetInfo(g_rx_ringbuffer, NULL, NULL, NULL, NULL, &items_waiting);
#if (defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS) || defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K))
    // if(items_waiting > 0){
    while (1) {
        xSemaphoreTake(g_rx_mux, portMAX_DELAY);
        char *item = (char *)xRingbufferReceive(g_rx_ringbuffer, &item_size, pdMS_TO_TICKS(10));
        if (item != NULL) {
            if(size >= item_size){
                memcpy(data, item, item_size);
            }else{
                ESP_LOGE(TAG, "size is too small");
                item_size = 0;
            }
            vRingbufferReturnItem(g_rx_ringbuffer, (void *)item);
            xSemaphoreGive(g_rx_mux);
            break;
        }
        xSemaphoreGive(g_rx_mux);
    }
    // }
#else
    // if(items_waiting > size){
    while (1) {
        xSemaphoreTake(g_rx_mux, portMAX_DELAY);
        char *item = (uint8_t *)xRingbufferReceiveUpTo(g_rx_ringbuffer, &item_size, pdMS_TO_TICKS(10), size);
        if(item_size > 0){
            memcpy(data, item, item_size);
            vRingbufferReturnItem(g_rx_ringbuffer, (void *)item);
            xSemaphoreGive(g_rx_mux);
            break;
        }
        xSemaphoreGive(g_rx_mux);
    }
    // }
#endif
    return item_size;
}

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1

static void __start_ota_timer_cb(void* arg){
    if (s_mode != VB6824_MODE_OTA)
    {
        uint8_t ota_data = 0x01;
        __frame_send(VB6824_CMD_SEND_OTA, &ota_data, 1);
        esp_timer_start_once(start_ota_timer, 500000);
    }
}

int vb6824_ota(const char* code, jl_ota_event_t evt_cb){
    ESP_LOGW(TAG, "vb6824_ota:%s",  code);
    s_ota_evt = evt_cb;    
    jl_ota_set_code(code);
    if (s_wait_fresh_wakeup_word)
    {
        if (g_voice_event_cb)
        {
            g_voice_event_cb(VB6824_EVT_OTA_START, 0, g_voice_event_cb_arg);
        }
        
        jl_ota_start(vb_ota_evt_cb);
        s_mode = VB6824_MODE_OTA;
        return 1;
    }
    s_wait_fresh_wakeup_word = 1;
    uint8_t ota_data = 0x01;
    __frame_send(VB6824_CMD_SEND_OTA, &ota_data, 1);
    if (start_ota_timer ==  NULL)
    {    
        esp_timer_create_args_t timer_args = {
            .callback = __start_ota_timer_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "start_ota",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &start_ota_timer);
    }

    if(start_ota_timer){
        esp_timer_start_once(start_ota_timer, 500000);
    }else{
        ESP_LOGE(TAG, "send_timer is null");
        return 0;
    }
    
    return 1;
}


void __check_vb_timer_cb(void *arg){
    static uint8_t times = 0;
    if (s_wait_fresh_wakeup_word)
    {
        if (times>=20)
        {

            if(s_wait_vb_hello && g_voice_event_cb){
                g_voice_event_cb(VB6824_EVT_OTA_ENTER, 1, g_voice_event_cb_arg);
            }
            return;
        }
        times++;
        uint8_t test = 1;
        __frame_send(VB6824_CMD_SEND_GET_WAKEUP_WORD, &test, 1);
        esp_timer_start_once(check_wakeword, 200*1000);
    }
    
}
#endif

bool vb6824_is_support_ota(){
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    if (s_wait_vb_hello==0 && s_wait_fresh_wakeup_word==1)
    {
        return false;
    }
    return true;
#else
    return false;
#endif
}

void vb6824_init(gpio_num_t tx, gpio_num_t rx){
    __uart_init(tx, rx);

#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    jl_set_uart_port(UART_NUM);
#endif

    g_rx_mux = xSemaphoreCreateMutex();

#if defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS)
    g_rx_ringbuffer = xRingbufferCreate(RECV_BUF_LENGTH, RINGBUF_TYPE_NOSPLIT);
    g_tx_ringbuffer = xRingbufferCreate(SEND_BUF_LENGTH, RINGBUF_TYPE_NOSPLIT);
#elif defined(CONFIG_VB6824_TYPE_OPUS_16K_20MS_PCM_16K)
    g_rx_ringbuffer = xRingbufferCreate(RECV_BUF_LENGTH, RINGBUF_TYPE_NOSPLIT);
    g_tx_ringbuffer = xRingbufferCreate(SEND_BUF_LENGTH, RINGBUF_TYPE_BYTEBUF);
#else
    g_rx_ringbuffer = xRingbufferCreate(RECV_BUF_LENGTH, RINGBUF_TYPE_BYTEBUF);
    g_tx_ringbuffer = xRingbufferCreate(SEND_BUF_LENGTH, RINGBUF_TYPE_BYTEBUF);
#endif

    uint8_t test = 1;
    __frame_send(VB6824_CMD_SEND_GET_WAKEUP_WORD, &test, 1);
    
#if defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1
    esp_timer_create_args_t check_timer_args = {
        .callback = __check_vb_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vb_check",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&check_timer_args, &check_wakeword);
    if(check_wakeword){
        esp_timer_start_once(check_wakeword, 200*1000);
    }else{
        ESP_LOGE(TAG, "send_timer is null");
    }
#endif
#ifdef CONFIG_VB6824_SEND_USE_TASK
    xTaskCreate(__send_task, "__send_task", CONFIG_VB6824_SEND_TASK_STACK_SIZE, NULL, 9, NULL);
#else
    esp_timer_handle_t send_timer = NULL;
    esp_timer_create_args_t timer_args = {
        .callback = __send_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vb_send",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &send_timer);
    if(send_timer){
        esp_timer_start_periodic(send_timer, AUDIO_SEND_CHENK_MS*1000);
    }else{
        ESP_LOGE(TAG, "send_timer is null");
    }
#endif
}