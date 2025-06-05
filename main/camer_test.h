// ESP32-S3 Camera Display via LVGL UI (Full Example)

#include <esp_camera.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <vector>
#include <img_converters.h>
#include "lvgl.h"
#define TAG "camer_test"
// External LVGL root container
// extern lv_obj_t* ui_root_container;

// Camera & Display globals
static bool camera_mode = false;
static lv_obj_t *cam_img = nullptr;
static lv_img_dsc_t cam_img_dsc;
static uint8_t *cam_buffer = nullptr;
static const int cam_width = 320;
static const int cam_height = 240;
static TaskHandle_t camera_task_handle = nullptr;
static camera_fb_t *fb = nullptr;
static SemaphoreHandle_t cam_img_mutex = xSemaphoreCreateMutex();
SemaphoreHandle_t fb_mutex = xSemaphoreCreateMutex();
SemaphoreHandle_t cam_buffer_mutex = xSemaphoreCreateMutex();
SemaphoreHandle_t camera_lvgl_task_mutex = xSemaphoreCreateMutex();

// Create LVGL image object for camera frame
int create_camera_lvgl_display(lv_obj_t *parent)
{
    if (xSemaphoreTake(camera_lvgl_task_mutex, 30) == pdTRUE)
    {
        if (cam_img)
        {
            lv_obj_clear_flag(cam_img, LV_OBJ_FLAG_HIDDEN);
            return -1;
        }

        cam_buffer = (uint8_t *)heap_caps_malloc(cam_width * cam_height * 2, MALLOC_CAP_SPIRAM);
        if (!cam_buffer)
            return -1;
        ESP_LOGW(TAG, "start _camer_test");

        cam_img_dsc.header.w = cam_width;
        cam_img_dsc.header.h = cam_height;
        cam_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        cam_img_dsc.data_size = cam_width * cam_height * 2;
        cam_img_dsc.data = cam_buffer;
        if (xSemaphoreTake(cam_img_mutex, portMAX_DELAY) == pdTRUE)
        {
            if (parent)
            {
                cam_img = lv_img_create(parent);
                ESP_LOGW(TAG, "lv_img_createparent————————————ok");
            }
            else
            {
                ESP_LOGW(TAG, "lv_img_createparent————————————worry");

                return -1;
            }

            lv_obj_align(cam_img, LV_ALIGN_CENTER, 0, 0);
            xSemaphoreGive(cam_img_mutex);
            ESP_LOGI(TAG, "create_camera_lvgl_displaycam_img_mutex");
        }
        xSemaphoreGive(camera_lvgl_task_mutex);
    }
    return 0;

    // lv_img_set_src(cam_img, &cam_img_dsc);
    // lv_obj_align(cam_img, LV_ALIGN_CENTER, 0, 0);
    // lv_obj_move_foreground(cam_img);
}
void covert(camera_fb_t *fb_, uint8_t *cam_buffer)
{
    if (!cam_buffer)
        return;
    if (!fb_)
        return;

    auto src = (uint16_t *)fb_->buf;
    auto dst = (uint16_t *)cam_buffer;
    size_t pixel_count = fb_->len / 2;
    for (size_t i = 0; i < pixel_count; i++)
    {
        // 交换每个16位字内的字节
        dst[i] = __builtin_bswap16(src[i]);
    }
    ESP_LOGI(TAG, "start _covert_image____ok");
}
// Camera image refresh task
void camera_lvgl_task(void *param)
{
    ESP_LOGI(TAG, "start_camera_lvgl_task_____________________1");
    if (xSemaphoreTake(camera_lvgl_task_mutex, portMAX_DELAY) == pdTRUE)
    {
        while (camera_mode)
        {

            //  camera_fb_t* fb = esp_camera_fb_get();
            //  if(!fb){
            //     ESP_LOGW(TAG,"esp_camera_fb_get_____________________error");
            //  }
            for (int i = 0; i < 2; i++)
            {
                if (fb != nullptr)
                {
                    esp_camera_fb_return(fb);
                }
                fb = esp_camera_fb_get();
                if (fb == nullptr)
                {
                    ESP_LOGE(TAG, "Camera capture failed");
                    return;
                }
            }

            // ESP_LOGI(TAG,"cam_width:%d",fb->width);
            if (fb)
            {
                if (xSemaphoreTake(cam_buffer_mutex, portMAX_DELAY) == pdTRUE)
                {
                    covert(fb, cam_buffer);
                    xSemaphoreGive(cam_buffer_mutex);
                }

                ESP_LOGI(TAG, "start_camera_lvgl_task_____________________2");
                // memcpy(cam_buffer, fb->buf, cam_width * cam_height * 2);
                vTaskDelay(pdMS_TO_TICKS(10));

                if (xSemaphoreTake(cam_img_mutex, 30) == pdTRUE)
                {

                    if (cam_img)
                    {
                        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
                        // 更新图像内容
                        lv_img_set_src(cam_img, &cam_img_dsc);
                        ESP_LOGI(TAG, "Free internal2: %u minimal internal: %u", free_sram, min_free_sram);
                        // lv_obj_invalidate(cam_img);
                        //  ESP_LOGI(TAG, "Free internal3: %u minimal internal: %u", free_sram, min_free_sram);
                    }
                    xSemaphoreGive(cam_img_mutex);
                }
                else
                {
                    ESP_LOGI(TAG, "cam_img_mutex__not");
                }

                //  if (cam_img) {
                //             // 更新图像内容
                //           lv_img_set_src(cam_img, &cam_img_dsc);

                //  }else{
                //          ESP_LOGI(TAG,"cam_img_____________________not");
                //  }

                ESP_LOGI(TAG, "end_camera_lvgl_task_____________________3");
                // lv_obj_invalidate(cam_img);
                esp_camera_fb_return(fb);

                ESP_LOGI(TAG, "end_camera_lvgl_task");
                if (!camera_mode)
                {

                    break;
                }
            }
            else
            {

                if (fb)
                    esp_camera_fb_return(fb);
                if (!camera_mode)
                {

                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    ESP_LOGW(TAG, "end_camera_lvgl_task____________4vTaskDelete");
    xSemaphoreGive(camera_lvgl_task_mutex);
    vTaskDelete(nullptr);
}

void show_camera_feed(lv_obj_t *ui_root_container)
{
    camera_mode = true;
    // lv_obj_add_flag(ui_root_container, LV_OBJ_FLAG_HIDDEN);
    // lv_obj_add_flag(ui_root_container, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "end_camera_lvgl_task——————————————————————overflow");

    if (create_camera_lvgl_display(ui_root_container) < 0)
    {
        ESP_LOGW(TAG, "create_camera_lvgl_display-------error");
    };
    if (camera_task_handle)
    {
        ESP_LOGW(TAG, "camera_task_handle-------error");
    }
    else
    {
        ESP_LOGI(TAG, "camera_task_handle------0k");
    }
    xTaskCreate(camera_lvgl_task, "cam_lvgl_task", 4096, nullptr, 3, &camera_task_handle);
}

// Stop camera and restore UI
void hide_camera_feed(lv_obj_t *ui_root_container)
{
    ESP_LOGI(TAG, "Hide_camera_lvgl_task_________start1");
    camera_mode = false;
    if (xSemaphoreTake(camera_lvgl_task_mutex, portMAX_DELAY) == pdTRUE)
    {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (camera_task_handle)
        {

            // vTaskDelete(camera_task_handle);
            camera_task_handle = nullptr;
            ESP_LOGI(TAG, "camera_task_handle_camera_lvgl_task_________start2");
        }
        if (cam_img)
        {
            // vTaskDelay(pdMS_TO_TICKS(30));
            ESP_LOGI(TAG, "cam_img_lvgl_task_________start3");

            if (xSemaphoreTake(cam_img_mutex, 30) == pdTRUE)
            {
                if (cam_img)
                {
                    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                    ESP_LOGI(TAG, "hide——Free internal: %u minimal internal: %u", free_sram, min_free_sram);
                    // 更新图像内容
                    lv_obj_add_flag(cam_img, LV_OBJ_FLAG_HIDDEN);
                    // lv_obj_del(cam_img);
                    ESP_LOGI(TAG, "hide2——Free internal: %u minimal internal: %u", free_sram, min_free_sram);
                    cam_img = nullptr;
                }
                xSemaphoreGive(cam_img_mutex);
            }
            else
            {
                ESP_LOGW(TAG, "cam_img_lvgl_task_________sisuo");
            }

            if (cam_buffer)
            {
                if (xSemaphoreTake(cam_buffer_mutex, portMAX_DELAY) == pdTRUE)
                {
                    // free(cam_buffer);
                    heap_caps_free((void *)cam_buffer);
                    cam_buffer = nullptr;
                    xSemaphoreGive(cam_buffer_mutex);

                    ESP_LOGI(TAG, "cam_buffer_lvgl_task_________start4");
                }
            }

            // lv_obj_del(cam_img);
            // cam_img = nullptr;
            ESP_LOGI(TAG, "cam_img_lvgl_task_________end");
        }

        ESP_LOGI(TAG, "Hide_camera_lvgl_task______________end");
        xSemaphoreGive(camera_lvgl_task_mutex);

        // lv_obj_clear_flag(cam_img, LV_OBJ_FLAG_HIDDEN);
    }
}

// // Toggle camera mode
// void toggle_camera_mode() {
//     if (camera_mode) {
//         hide_camera_feed();
//     } else {
//         show_camera_feed();
//     }
// }
