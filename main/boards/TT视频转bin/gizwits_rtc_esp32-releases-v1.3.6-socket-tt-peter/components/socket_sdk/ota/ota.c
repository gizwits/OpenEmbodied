#include "esp_netif.h"

#include "audio_mem.h"
#include "ota_service.h"
#include "ota_proc_default.h"
#include "esp_log.h"
#include "config.h"
#include "ota.h"
static const char *TAG = "GIZWITS_OTA";
static EventGroupHandle_t events = NULL;
static bool is_ota_running = false;
static char current_sw_ver[32] = {0};
static char current_url[256] = {0};

#define OTA_FINISH (BIT0)


static ota_service_err_reason_t need_upgrade(void *handle, ota_node_attr_t *node)
{
    // ESP_LOGE(TAG, "data partition [%s] not found", node->label);
    return OTA_SERV_ERR_REASON_SUCCESS;
}

static esp_err_t ota_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    if (evt->type == OTA_SERV_EVENT_TYPE_RESULT) {
        ota_result_t *result_data = evt->data;
        if (result_data->result != ESP_OK) {
            send_trace_log("OTA失败", "");
            ESP_LOGE(TAG, "List id: %d, OTA failed", result_data->id);
        } else {
            send_trace_log("OTA成功", "");
            ESP_LOGI(TAG, "List id: %d, OTA success", result_data->id);
        }
    } else if (evt->type == OTA_SERV_EVENT_TYPE_FINISH) {
        xEventGroupSetBits(events, OTA_FINISH);
    }

    return ESP_OK;
}

static void progress_callback(int progress) {
    ESP_LOGI(TAG, "OTA Progress: %d%%", progress);
    // 格式化字符串
    char progress_str[16];
    snprintf(progress_str, sizeof(progress_str), "%d%%", progress);
    send_trace_log("OTA进度", progress_str);
}

static void finish_callback(void) {
    ESP_LOGI(TAG, "OTA Finished successfully.");
    send_trace_log("OTA完成", "");
}

static void error_callback(esp_err_t err) {
    ESP_LOGE(TAG, "OTA Error: %s", esp_err_to_name(err));
    send_trace_log("OTA失败", esp_err_to_name(err));
}

static void cancel_callback(void) {
    ESP_LOGI(TAG, "OTA Cancelled.");
    send_trace_log("OTA取消", "");
}
void start_ota(void *params) {

    ESP_LOGI(TAG, "start ota %s", current_url);
    send_trace_log("开始OTA", current_url);

    // 判断版本号是否小于当前版本号
    if (strcmp(current_sw_ver, sdk_get_software_version()) <= 0) {
        ESP_LOGI(TAG, "version is less than current version");
        is_ota_running = false;
        return;
    }

    ota_service_config_t ota_service_cfg = OTA_SERVICE_DEFAULT_CONFIG();
    ota_service_cfg.task_stack = 8 * 1024;
    ota_service_cfg.evt_cb = ota_service_cb;
    ota_service_cfg.cb_ctx = NULL;
    periph_service_handle_t ota_service = ota_service_create(&ota_service_cfg);
    events = xEventGroupCreate();

    ESP_LOGI(TAG, "Set upgrade list");
    send_trace_log("设置升级列表", "");
    ota_upgrade_ops_t upgrade_list[] = {
        {
            {
                ESP_PARTITION_TYPE_APP,
                NULL,
                current_url,
                NULL
            },
            progress_callback, // 添加进度回调
            finish_callback,   // 添加完成回调
            error_callback,    // 添加错误回调
            cancel_callback,   // 添加取消回调
            true,
            false
        }
    };

    ota_app_get_default_proc(&upgrade_list[0]);
    upgrade_list[0].need_upgrade = need_upgrade;

    ota_service_set_upgrade_param(ota_service, upgrade_list, sizeof(upgrade_list) / sizeof(ota_upgrade_ops_t));

    ESP_LOGI(TAG, "Start OTA service");
    AUDIO_MEM_SHOW(TAG);
    periph_service_start(ota_service);

    EventBits_t bits = xEventGroupWaitBits(events, OTA_FINISH, true, false, portMAX_DELAY);
    if (bits & OTA_FINISH) {
        ESP_LOGI(TAG, "Finish OTA service");
        send_trace_log("OTA完成", "");
    }
    ESP_LOGI(TAG, "Clear OTA service");
    periph_service_destroy(ota_service);
    vEventGroupDelete(events);
    is_ota_running = false;
}

void run_start_ota_task(char *sw_ver, char *url) {
    if (is_ota_running) {
        send_trace_log("OTA正在运行，忽略新请求", "");
        ESP_LOGW(TAG, "OTA is already running, ignoring new request");
        return;
    }

    if (!sw_ver || !url) {
        send_trace_log("OTA 无效参数", "");
        ESP_LOGE(TAG, "Invalid parameters");
        return;
    }

    // 拷贝字符串
    strncpy(current_sw_ver, sw_ver, sizeof(current_sw_ver) - 1);
    current_sw_ver[sizeof(current_sw_ver) - 1] = '\0';
    
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    printf("module version: %s\n", current_sw_ver);
    printf("https link: %s\n", current_url);
    
    is_ota_running = true;
    xTaskCreatePinnedToCore(start_ota, "start_ota", 4096, NULL, 5, NULL, 0);
}