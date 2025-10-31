#include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "http_stream.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "mp3_decoder.h"
#include "board.h"
#include "algorithm_stream.h"
#include "filter_resample.h"

// #include "esp_peripherals.h"
// #include "periph_sdcard.h"
#include "i2s_stream.h"
#include "pthread.h"

#include "esp_timer.h"
#include "esp_partition.h"
#include "audio_log.h"
#include "xtask.h"

static const char *TAG = "audio_log";

const char *serverUrl = "http://192.168.1.107:8000";

#ifdef CONFIG_RTC_AUDIO_SAVE_TO_FLASH

// Add these global variables at the top of the file after other includes
static bool s_enable_log = false;
static uint32_t s_flash_offset = 0;
static uint8_t *s_flash_buffer = NULL;
// static const size_t FLASH_BUFFER_SIZE = 512 * 1024; // 512KB buffer
static const size_t FLASH_BUFFER_SIZE = 2 * 1024 * 1024; // 1MB buffer

// 初始化日志
int audio_log_init() {
    // Initialize buffer on first write
    if (!s_flash_buffer) {
        s_flash_buffer = heap_caps_malloc(FLASH_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (s_flash_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate flash buffer");
            return -1;
        }
    } else {
        ESP_LOGE(TAG, "=============== RESET FLASH BUFFER ===============");
    }


    // 初始化状态
    s_flash_offset = 0;
    s_enable_log = true;
    ESP_LOGW(TAG, "=============== audio_log_init done: s_enable_log = %d, s_flash_offset = 0x%lx ===============", s_enable_log, s_flash_offset);

    return 0;
}

void audio_log_enable() {
    ESP_LOGE(TAG, "audio_log_enable");
    s_enable_log = true;
}

void audio_log_disable() {
    ESP_LOGE(TAG, "audio_log_disable");
    s_enable_log = false;
}

void audio_log_toggle() {
    ESP_LOGI(TAG, "audio_log_toggle");
    if (s_enable_log) {
        ESP_LOGI(TAG, "audio_log_toggle: switch to disable");
        audio_log_end();
    } else {
        ESP_LOGI(TAG, "audio_log_toggle: switch to enable");
    }

    // 切换状态
    s_enable_log = !s_enable_log;
}

// 写入flash
void audio_write_buffer_to_flash() {

    // Add flash writing code before cleanup
    if (s_flash_buffer && s_flash_offset > 0) {
        
        // Find the log partition
        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "log_data");
            
        if (partition) {

            ESP_LOGI(TAG, "Erasing partition: 0x%lx", partition->address);

            // Erase the partition first
            ESP_ERROR_CHECK(esp_partition_erase_range(partition, 0, partition->size));
            
            ESP_LOGI(TAG, "Writing 0x%lx bytes data to partition: 0x%lx", s_flash_offset, partition->address);

            // Write the data
            esp_err_t err = esp_partition_write(partition, 0, s_flash_buffer, s_flash_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write to flash: %d", err);
            } else {
                ESP_LOGI(TAG, "Successfully wrote data to flash");
            }
        } else {
            ESP_LOGE(TAG, "Could not find log partition");
        }

        // Print first 32 bytes and last 32 bytes
        ESP_LOGI(TAG, "First 32 bytes:");
        for (int i = 0; i < 32; i += 2) {
            printf("0x%02x%02x ", s_flash_buffer[i], s_flash_buffer[i+1]);
        }
        printf("\n");

        ESP_LOGI(TAG, "Last 32 bytes:");
        for (int i = s_flash_offset - 32; i < s_flash_offset; i += 2) {
            printf("0x%02x%02x ", s_flash_buffer[i], s_flash_buffer[i+1]);
        }
        printf("\n");
        
        // Free the buffer
        s_enable_log = false;
        free(s_flash_buffer);
        s_flash_buffer = NULL;
    }
}

// 添加数据到flash缓冲区
int audio_append_to_flash_buffer(char *buf, int len)
{
    if (!s_enable_log) {
        return 0;
    }

    if (s_flash_buffer == NULL) {
        ESP_LOGE(TAG, "s_flash_buffer is NULL");
        return 0;
    }

    // Check if buffer has enough space
    if (s_flash_offset + len > FLASH_BUFFER_SIZE) {
        // ESP_LOGW(TAG, "Flush buffer to flash");
        // audio_write_buffer_to_flash();
        // ESP_LOGW(TAG, "Flush buffer to flash done");
        // s_flash_offset = 0;
        // while (1) {
        //     vTaskDelay(pdMS_TO_TICKS(1000));
        // }
        
        return len;
    }

    // Copy data to buffer
    memcpy(s_flash_buffer + s_flash_offset, buf, len);
    s_flash_offset += len;
    return len;
}

// 添加分隔符
void audio_log_append_seperator() {
    static char sep[200] = {0};

    memset(sep, -1000, sizeof(sep));
    audio_append_to_flash_buffer(sep, sizeof(sep));
}

// 结束日志, 写入flash
void audio_log_end() {
    if (!s_enable_log) {
        ESP_LOGI(TAG, "audio_log_end: need to enable log first");
        return;
    }

    // user_byte_rtc_leave_room();
    // extern recorder_pipeline_handle_t recorder_pipeline;
    // recorder_pipeline_stop(recorder_pipeline);
    ESP_LOGW(TAG, "=============== Flush buffer to flash: s_flash_offset = 0x%lx ===============", s_flash_offset);
    audio_write_buffer_to_flash();
    ESP_LOGW(TAG, "=============== Flush buffer to flash done ===============");
    // while (1) {
    //     vTaskDelay(pdMS_TO_TICKS(1000));
    // }
}

#endif

#ifdef CONFIG_RTC_AUDIO_SAVE_TO_SERVER

// 音频数据缓冲链表的头指针
static volatile AudioBuffer *audioBuffers = NULL;
static volatile int audioBufferTotalCnt = 0;
static volatile int audioBufferQueuedCnt = 0;
static SemaphoreHandle_t bufferMutex = NULL;  // 用于保护链表操作的互斥锁

static volatile uint8_t* s_raw_buffer = NULL;
static volatile uint8_t* s_aec_buffer = NULL;

// 初始化缓冲区
void initAudioBuffer() {
    if (bufferMutex == NULL) {
        bufferMutex = xSemaphoreCreateMutex();
        audioBufferTotalCnt = 0;
        audioBufferQueuedCnt = 0;
    }
}

// 为指定的 type 创建一个新的缓冲区
static AudioBuffer* createAudioBuffer(AudioBufferType type) {
    AudioBuffer *buffer = heap_caps_malloc(sizeof(AudioBuffer), MALLOC_CAP_SPIRAM);
    if (buffer) {
        buffer->type = type;
        buffer->size = 0;
        buffer->capacity = AUDIO_BUFFER_SIZE;
        buffer->next = NULL;
    }
    return buffer;
}

static void freeAudioBuffer(AudioBuffer* buffer) {
    if (buffer) {
        free(buffer);
    }
}

static AudioBuffer* getBufferByType(AudioBufferType type) {
    switch (type) {
        case AUDIO_BUFFER_TYPE_RAW:
            if (s_raw_buffer == NULL) {
                s_raw_buffer = createAudioBuffer(type);
            }
            return s_raw_buffer;
        case AUDIO_BUFFER_TYPE_AEC:
            if (s_aec_buffer == NULL) {
                s_aec_buffer = createAudioBuffer(type);
            }
            return s_aec_buffer;
        default:
            return NULL;
    }
}

static void reallocBuffer(AudioBufferType type) {
    switch (type) {
        case AUDIO_BUFFER_TYPE_RAW:
            s_raw_buffer = createAudioBuffer(type);
            break;
        case AUDIO_BUFFER_TYPE_AEC:
            s_aec_buffer = createAudioBuffer(type);
            break;
        default:
            break;
    }
}
// 将音频数据缓存在指定 type 中
void bufferAudioChunk(const uint8_t *data, size_t size, AudioBufferType type) {
    if (bufferMutex == NULL) {
        initAudioBuffer();
    }

    xSemaphoreTake(bufferMutex, portMAX_DELAY);

    // 获取对应类型的缓冲区
    AudioBuffer *buffer = getBufferByType(type);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to get buffer for type %d", type);
        xSemaphoreGive(bufferMutex);
        return;
    }

    // 检查缓冲区剩余空间
    if (buffer->size + size > buffer->capacity) {
        // 如果写满，将当前缓冲区加入链表
        if (audioBuffers == NULL) {
            audioBuffers = buffer;
        } else {
            AudioBuffer *last = audioBuffers;
            while (last->next != NULL) {
                last = last->next;
            }
            last->next = buffer;
        }
        audioBufferTotalCnt++;
        audioBufferQueuedCnt++;

        // 重新分配新的缓冲区
        reallocBuffer(type);
        printf("@");
    }

    // 添加数据到缓冲区
    printf("^");
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;

    xSemaphoreGive(bufferMutex);
}

void uploadAudioChunk(uint8_t *data, size_t size, const char *type) {
    char fullUrl[256]; 
    snprintf(fullUrl, sizeof(fullUrl), "%s/%s", serverUrl, type);

    esp_http_client_config_t config = {
        .url = fullUrl,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_post_field(client, (const char *)data, size);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Upload success, HTTP Status: %d", status_code);
    } else {
        ESP_LOGE(TAG, "Upload failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static char* getBufferTypeStr(AudioBufferType type) {
    switch (type) {
        case AUDIO_BUFFER_TYPE_RAW:
            return "raw";
        case AUDIO_BUFFER_TYPE_AEC:
            return "aec";
        default:
            return "unknown";
    }
}

void uploadBufferedAudioTask(void *param) {
    while (1) {
        xSemaphoreTake(bufferMutex, portMAX_DELAY);

        while (audioBuffers != NULL) {
        AudioBuffer *buffer = audioBuffers;
            // 如果缓冲区的数据足够大，进行上传
            if (buffer->size > 0) {
                audioBufferQueuedCnt--;

                ESP_LOGW(TAG, "Uploading audio: queued %d / total %d, size: %d", audioBufferQueuedCnt, audioBufferTotalCnt, buffer->size);
                audioBuffers = buffer->next;
                xSemaphoreGive(bufferMutex);
                uploadAudioChunk(buffer->data, buffer->size, getBufferTypeStr(buffer->type));
                xSemaphoreTake(bufferMutex, portMAX_DELAY);
            }
            freeAudioBuffer(buffer);
        }

        xSemaphoreGive(bufferMutex);
        vTaskDelay(pdMS_TO_TICKS(200));  // 每 200ms检查一次
    }
}

void startAudioUploadService() {
    ESP_LOGW(TAG, "Start audio upload service");
    xTaskCreateExt(uploadBufferedAudioTask, "UploadBufferedAudio", 16*1024, NULL, 5, NULL);
}
#endif
