#include "video_player.h"
#include "w25q64_flash.h"
#include "board.h"
#include "eye_display.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "VideoPlayer";

int VideoPlayer::DefaultEmotionToGroup(const std::string& name) {
    ESP_LOGI(TAG, "DefaultEmotionToGroup: 开始映射表情 '%s'", name.c_str());
    // 状态映射，和 SetEmotion 支持的状态保持一致
    // 将相似的状态映射到同一个视频组
    
    // 组 0: 开心相关
    if (name == "happy") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 0", name.c_str());
        return 0;
    }
    if (name == "laughing") return 0;
    if (name == "funny") return 0;
    if (name == "cool") return 0;
    if (name == "relaxed") return 0;
    if (name == "confident") return 0;
    
    // 组 1: 中性/待机
    if (name == "neutral") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 1", name.c_str());
        return 1;
    }
    
    // 组 2: 悲伤相关
    if (name == "sad") return 2;
    if (name == "crying") return 2;
    
    // 组 3: 惊讶相关
    if (name == "surprised") return 3;
    if (name == "surprise") return 3;  // 兼容旧名称
    if (name == "shocked") return 3;
    if (name == "embarrassed") return 3;
    if (name == "delicious") return 3;
    if (name == "confused") return 3;
    
    // 组 4: 愤怒
    if (name == "angry") return 4;
    
    // 组 5: 爱心/亲密相关
    if (name == "loving") return 5;
    if (name == "kissy") return 5;
    
    // 组 6: 思考
    if (name == "thinking") return 6;
    
    // 组 7: 眨眼
    if (name == "winking") return 7;
    
    // 组 8: 睡眠
    if (name == "sleepy") return 8;
    
    // 组 9: 傻笑
    if (name == "silly") return 9;
    
    // 组 10: 眩晕
    if (name == "vertigo") return 10;
    
    // 组 11: 聆听（如果有的话）
    if (name == "listen") return 11;

    // 组 12 左转
    if (name == "Turn_left") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 12 (左转表情)", name.c_str());
        return 12;
    }

    // 组 13 右转
    if (name == "Turn_right") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 13 (右转表情)", name.c_str());
        return 13;
    }
        
    // 组 14 加速（前进倾角）
    if (name == "Accelerate") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 14 (加速表情，对应前进倾角)", name.c_str());
        return 14;
    }

    // 组 15 急刹（后退倾角）
    if (name == "Decelerate") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 15 (急刹表情，对应后退倾角)", name.c_str());
        return 15;
    }

    // 组 16 充电
    if (name == "Charging") {
        ESP_LOGI(TAG, "DefaultEmotionToGroup: '%s' -> group 16", name.c_str());
        return 16;
    }
        
    // 默认返回开心组
    ESP_LOGW(TAG, "DefaultEmotionToGroup: 未找到表情 '%s' 的映射，使用默认组 0", name.c_str());
    return 0;
}

VideoPlayer::VideoPlayer(int display_width, int display_height, int frame_delay_ms)
    : display_width_(display_width)
    , display_height_(display_height)
    , frame_delay_ms_(frame_delay_ms)
    , video_playing_(false)
    , video_group_index_(0)
    , video_img_(nullptr)
    , emotion_to_group_func_(DefaultEmotionToGroup)
    , last_start_tick_(0)
    , last_started_group_(-1) {
}

VideoPlayer::~VideoPlayer() {
    StopPlayback();
    if (video_img_ != nullptr) {
        if (lvgl_port_lock(1000)) {
            lv_obj_del(video_img_);
            lvgl_port_unlock();
        }
    }
}

int VideoPlayer::ReadVideoGroupCount() {
    auto& flash = W25Q64Flash::GetInstance();
    if (!flash.IsInitialized()) {
        ESP_LOGE(TAG, "External flash not initialized");
        return 0;
    }
    uint8_t cnt = 0;
    if (flash.Read(0, &cnt, 1) != ESP_OK) return 0;
    return (int)cnt;
}

void VideoPlayer::PlayVideoGroup(const char* emotion) {
    ESP_LOGI(TAG, "=== VideoPlayer::PlayVideoGroup 开始 ===");
    ESP_LOGI(TAG, "PlayVideoGroup: emotion='%s'", emotion ? emotion : "nullptr");
    
    if (emotion == nullptr) {
        ESP_LOGE(TAG, "PlayVideoGroup: emotion is nullptr");
        return;
    }
    
    int group_index = emotion_to_group_func_(std::string(emotion));
    ESP_LOGI(TAG, "PlayVideoGroup: emotion='%s' -> group_index=%d", emotion, group_index);
    PlayVideoGroupByIndex(group_index);
    ESP_LOGI(TAG, "PlayVideoGroup: 完成");
}

void VideoPlayer::PlayVideoGroupByIndex(int group_index) {
    ESP_LOGI(TAG, "=== VideoPlayer::PlayVideoGroupByIndex 开始 ===");
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: group_index=%d", group_index);
    
    int cnt = ReadVideoGroupCount();
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: 视频组数量=%d", cnt);
    
    if (cnt <= 0) {
        ESP_LOGE(TAG, "PlayVideoGroupByIndex: 没有可用的视频组 (cnt=%d)", cnt);
        return;
    }
    if (group_index < 0 || group_index >= cnt) {
        ESP_LOGE(TAG, "PlayVideoGroupByIndex: 无效的组索引 group_index=%d (最大: %d)", group_index, cnt - 1);
        return;
    }
    
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: 设置 video_group_index_=%d, 当前 video_playing_=%d, video_task_handle_=%p", 
             group_index, video_playing_, video_task_handle_);
    
    // 如果正在播放相同的组且任务句柄有效，不需要重新启动
    // 注意：如果任务句柄为nullptr（任务被强制删除），即使组索引相同也要重新启动
    if (video_playing_ && video_group_index_ == group_index && video_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "PlayVideoGroupByIndex: 正在播放相同的组 %d，跳过", group_index);
        return;
    }
    
    // 如果任务句柄为nullptr但video_playing_为true，说明任务被强制删除，需要重置状态
    if (video_task_handle_ == nullptr && video_playing_) {
        ESP_LOGW(TAG, "PlayVideoGroupByIndex: 检测到任务句柄为nullptr但video_playing_为true，重置状态");
        video_playing_ = false;
    }
    
    video_group_index_ = group_index;
    
    // 如果正在播放不同的组，先停止
    if (video_playing_ && video_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "PlayVideoGroupByIndex: 正在播放不同的组，先停止");
        StopPlayback();
        // 等待更长时间，确保视频任务完全退出（因为任务可能在读取Flash或更新LVGL）
        vTaskDelay(pdMS_TO_TICKS(500));
        // 再次确认任务已停止
        if (video_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "PlayVideoGroupByIndex: 任务仍未退出，再次停止");
            StopPlayback();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: 调用 StartVideoPlayback()");
    StartVideoPlayback();
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: 完成");
}

void VideoPlayer::StartVideoPlayback() {
    ESP_LOGI(TAG, "=== VideoPlayer::StartVideoPlayback 开始 ===");
    ESP_LOGI(TAG, "StartVideoPlayback: video_group_index_=%d, video_playing_=%d, video_task_handle_=%p", 
             video_group_index_, video_playing_, video_task_handle_);
    
    // 重入保护：同一组1秒内的重复启动忽略，但如果任务已经被停止，允许重新启动
    TickType_t now = xTaskGetTickCount();
    if (last_started_group_ == video_group_index_ &&
        (now - last_start_tick_) < pdMS_TO_TICKS(1000) &&
        video_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "StartVideoPlayback: 重入保护，忽略重复启动 (last_started_group_=%d, 时间差=%lu ms, task_handle=%p)", 
                 last_started_group_, (now - last_start_tick_) * portTICK_PERIOD_MS, video_task_handle_);
        return;
    }
    
    // 若已有任务在跑，先停止并等待退出
    if (video_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "StartVideoPlayback: 已有任务在运行，先停止");
        // 保存当前组索引和回调信息，以便在强制删除后手动触发回调
        int old_group_index = video_group_index_;
        OnGroupFinishedCallback old_callback = on_group_finished_callback_;
        void* old_callback_arg = on_group_finished_arg_;
        bool was_cycling = (old_callback != nullptr);  // 如果有回调，说明可能在轮播
        
        video_playing_ = false;
        // 等待更长时间，确保任务完全退出（因为任务可能在读取Flash或更新LVGL）
        for (int i = 0; i < 150 && video_task_handle_ != nullptr; ++i) { // 最多等 1500ms（增加等待时间）
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (video_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "StartVideoPlayback: 任务未在预期时间内退出，强制清理并删除任务");
            // 强制删除任务（如果任务还在运行）
            TaskHandle_t old_handle = video_task_handle_;
            video_task_handle_ = nullptr;
            vTaskDelete(old_handle);
            // 等待一下，确保任务删除完成
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // 如果任务被强制删除且之前有回调（说明在轮播），手动触发回调继续轮播
            // 注意：回调会检查轮播状态并切换到下一组，所以即使组索引相同也没关系
            if (was_cycling && old_callback != nullptr) {
                ESP_LOGI(TAG, "StartVideoPlayback: 任务被强制删除，手动触发回调继续轮播 (old_group=%d, new_group=%d)", 
                         old_group_index, video_group_index_);
                // 延迟触发回调，确保任务完全删除
                vTaskDelay(pdMS_TO_TICKS(50));
                old_callback(old_callback_arg, old_group_index);
            }
        } else {
            ESP_LOGI(TAG, "StartVideoPlayback: 旧任务已停止");
        }
        // 再次确保句柄为nullptr和状态已重置
        video_task_handle_ = nullptr;
        video_playing_ = false;
    }
    
    video_playing_ = true;
    ESP_LOGI(TAG, "StartVideoPlayback: 创建视频播放任务，group_index=%d", video_group_index_);
    // 降低优先级到0（最低），避免阻塞音频任务（音频任务优先级通常更高）
    BaseType_t ret = xTaskCreate(VideoPlayTask, "video_play", 4096, this, 0, &video_task_handle_);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "StartVideoPlayback: 视频播放任务创建成功，task_handle=%p", video_task_handle_);
    } else {
        ESP_LOGE(TAG, "StartVideoPlayback: 视频播放任务创建失败，ret=%d", ret);
        video_playing_ = false;
    }
    last_started_group_ = video_group_index_;
    last_start_tick_ = now;
    ESP_LOGI(TAG, "StartVideoPlayback: 完成");
}

void VideoPlayer::StopVideoPlayback() {
    if (!video_playing_ && video_task_handle_ == nullptr) return;
    video_playing_ = false;
    // 等待任务自删除清理句柄
    // 增加等待时间，确保任务完全退出（因为任务可能在读取Flash或更新LVGL）
    for (int i = 0; i < 100 && video_task_handle_ != nullptr; ++i) { // 最多等 1000ms
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // 如果任务仍未退出，强制删除
    if (video_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "StopVideoPlayback: 任务未在预期时间内退出，强制删除任务");
        TaskHandle_t old_handle = video_task_handle_;
        video_task_handle_ = nullptr;
        vTaskDelete(old_handle);
    }
    video_task_handle_ = nullptr;
}

void VideoPlayer::StopPlayback() {
    StopVideoPlayback();
    // 清除充电圆环缓存
    cached_charging_arc_ = nullptr;
    // 隐藏视频图像，避免与Display动画重叠
    if (video_img_ != nullptr) {
        if (lvgl_port_lock(1000)) {
            // 检查对象是否仍然有效（可能已被配网模式等删除）
            if (lv_obj_is_valid(video_img_)) {
                // 隐藏视频图像
                lv_obj_add_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
                // 将视频图像移到后面，确保Display对象显示在最前面
                lv_obj_move_background(video_img_);
                // 清空图像数据，避免显示最后一帧
                video_img_dsc_.data = nullptr;
                video_img_dsc_.data_size = 0;
                lv_img_set_src(video_img_, &video_img_dsc_);
            } else {
                // 对象已无效，清空指针
                ESP_LOGW(TAG, "StopPlayback: 视频图像对象已无效，清空指针");
                video_img_ = nullptr;
            }
            lvgl_port_unlock();
        }
    }
}

void VideoPlayer::VideoPlayTask(void* arg) {
    auto* self = static_cast<VideoPlayer*>(arg);
    ESP_LOGI(TAG, "=== VideoPlayTask 开始 ===");
    ESP_LOGI(TAG, "VideoPlayTask: video_group_index_=%d", self->video_group_index_);
    
    // 使用外置 Flash
    auto& flash = W25Q64Flash::GetInstance();
    if (!flash.IsInitialized()) {
        ESP_LOGE(TAG, "VideoPlayTask: 外置Flash未初始化");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "VideoPlayTask: Flash已初始化");
    
    // Read header: 1 byte count + N*4 bytes frame counts
    uint8_t group_count = 0;
    if (flash.Read(0, &group_count, 1) != ESP_OK || group_count == 0) {
        ESP_LOGE(TAG, "VideoPlayTask: 无效的视频头 (group_count=%d)", group_count);
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "VideoPlayTask: 读取到视频组数量=%d", group_count);
    
    std::vector<uint32_t> frame_counts(group_count, 0);
    if (flash.Read(1, (uint8_t*)frame_counts.data(), group_count * sizeof(uint32_t)) != ESP_OK) {
        ESP_LOGE(TAG, "read frame counts failed");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // compute offsets
    const uint32_t frame_size = self->display_width_ * self->display_height_ * 2;
    uint32_t data_offset = 1 + group_count * sizeof(uint32_t);
    std::vector<uint32_t> group_base(group_count, 0);
    uint32_t acc_frames = 0;
    for (int i = 0; i < group_count; ++i) {
        group_base[i] = data_offset + acc_frames * frame_size;
        acc_frames += frame_counts[i];
    }
    
    int g = self->video_group_index_;
    ESP_LOGI(TAG, "VideoPlayTask: 原始 group_index=%d, group_count=%d", g, group_count);
    if (g < 0 || g >= group_count) {
        ESP_LOGW(TAG, "VideoPlayTask: group_index=%d 超出范围，重置为0", g);
        g = 0;
    }
    uint32_t frames = frame_counts[g];
    ESP_LOGI(TAG, "VideoPlayTask: 视频头信息 - groups=%u, frame_size=%u, data_offset=%u, play_group=%d, frames_in_group=%u, group_base=%u",
             (unsigned)group_count, (unsigned)frame_size, (unsigned)data_offset, g, (unsigned)frames, (unsigned)group_base[g]);
    
    if (frames == 0) {
        ESP_LOGE(TAG, "VideoPlayTask: 组 %d 的帧数为0，无法播放", g);
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // allocate frame buffer
    // Prefer DMA-capable internal memory for SPI DMA
    ESP_LOGI(TAG, "VideoPlayTask: 分配帧缓冲区，大小=%u", (unsigned)frame_size);
    uint8_t* buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)malloc(frame_size);
    if (!buf) {
        ESP_LOGE(TAG, "VideoPlayTask: 无法分配帧缓冲区内存");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "VideoPlayTask: 帧缓冲区分配成功，buf=%p", buf);
    
    // 先读取首帧并设置到图像，再显示，避免切组时先显示空白导致黑屏扫描
    uint32_t idx = 0;
    size_t off0 = group_base[g] + idx * frame_size;
    ESP_LOGI(TAG, "VideoPlayTask: 读取首帧，offset=%u, frame_size=%u", (unsigned)off0, (unsigned)frame_size);
    if (flash.Read(off0, buf, frame_size) != ESP_OK) {
        ESP_LOGE(TAG, "VideoPlayTask: 读取首帧失败，offset=%u, frame_size=%u", (unsigned)off0, (unsigned)frame_size);
        free(buf);
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "VideoPlayTask: 首帧读取成功");
    
    // 使用非阻塞锁直接更新 LVGL，复用固定缓冲区 buf，避免内存泄漏
    ESP_LOGI(TAG, "VideoPlayTask: 尝试获取LVGL锁（非阻塞，避免阻塞音频处理）");
    if (lvgl_port_lock(0)) {  // 非阻塞锁，立即返回，如果获取失败就跳过首帧显示
        ESP_LOGI(TAG, "VideoPlayTask: LVGL锁获取成功");
        lv_obj_t* screen = lv_screen_active();
        ESP_LOGI(TAG, "VideoPlayTask: screen=%p", screen);
        if (screen == nullptr) {
            ESP_LOGE(TAG, "VideoPlayTask: lv_screen_active() 返回 nullptr，无法创建视频图像");
            free(buf);
            self->video_playing_ = false;
            lvgl_port_unlock();
            vTaskDelete(nullptr);
            return;
        }
        
        ESP_LOGI(TAG, "VideoPlayTask: video_img_=%p", self->video_img_);
        if (self->video_img_ == nullptr) {
            ESP_LOGI(TAG, "VideoPlayTask: 创建新的视频图像对象");
            self->video_img_ = lv_image_create(screen);
            if (self->video_img_ == nullptr) {
                ESP_LOGE(TAG, "VideoPlayTask: 创建视频图像对象失败");
                free(buf);
                self->video_playing_ = false;
                lvgl_port_unlock();
                vTaskDelete(nullptr);
                return;
            }
            ESP_LOGI(TAG, "VideoPlayTask: 视频图像对象创建成功，video_img_=%p", self->video_img_);
            lv_obj_set_size(self->video_img_, self->display_width_, self->display_height_);
            // 完全居中显示
            lv_obj_align(self->video_img_, LV_ALIGN_CENTER, 0, 0);
            // 确保视频图像对象可见，设置背景色为透明
            lv_obj_set_style_bg_opa(self->video_img_, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
            // 确保视频图像在最前面
            lv_obj_move_foreground(self->video_img_);
            ESP_LOGI(TAG, "VideoPlayTask: 设置视频图像大小和位置完成");
        } else {
            ESP_LOGI(TAG, "VideoPlayTask: 使用已存在的视频图像对象");
            // 检查已存在的对象是否仍然有效（可能被配网模式等删除）
            if (!lv_obj_is_valid(self->video_img_)) {
                ESP_LOGW(TAG, "VideoPlayTask: 已存在的视频图像对象已无效，重新创建");
                self->video_img_ = lv_image_create(screen);
                if (self->video_img_ == nullptr) {
                    ESP_LOGE(TAG, "VideoPlayTask: 重新创建视频图像对象失败");
                    free(buf);
                    self->video_playing_ = false;
                    lvgl_port_unlock();
                    vTaskDelete(nullptr);
                    return;
                }
                lv_obj_set_size(self->video_img_, self->display_width_, self->display_height_);
                lv_obj_align(self->video_img_, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_bg_opa(self->video_img_, LV_OPA_TRANSP, 0);
                lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(self->video_img_);
                ESP_LOGI(TAG, "VideoPlayTask: 重新创建视频图像对象成功");
            }
        }
        ESP_LOGI(TAG, "VideoPlayTask: 设置视频图像描述符，width=%d, height=%d", 
                 self->display_width_, self->display_height_);
        self->video_img_dsc_.header.w = self->display_width_;
        self->video_img_dsc_.header.h = self->display_height_;
        self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        self->video_img_dsc_.data = buf;  // 直接使用固定缓冲区，避免内存分配
        self->video_img_dsc_.data_size = frame_size;
        
        ESP_LOGI(TAG, "VideoPlayTask: 设置图像源并显示");
        // 再次检查对象有效性（防止在设置过程中被删除）
        if (self->video_img_ == nullptr || !lv_obj_is_valid(self->video_img_)) {
            ESP_LOGW(TAG, "VideoPlayTask: 视频图像对象在设置前已无效，停止播放");
            free(buf);
            self->video_playing_ = false;
            lvgl_port_unlock();
            vTaskDelete(nullptr);
            return;
        }
        lv_img_set_src(self->video_img_, &self->video_img_dsc_);
        // 确保视频图像对象可见且在最前面
        lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->video_img_);
        // 强制刷新显示
        lv_obj_invalidate(self->video_img_);
        ESP_LOGI(TAG, "VideoPlayTask: 视频图像已显示，video_img_=%p, hidden=%d, parent=%p", 
                 self->video_img_, lv_obj_has_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_parent(self->video_img_));
        
        // 确保充电圆环始终显示在视频图像之上
        // 通过屏幕对象查找充电圆环（arc类型对象）
        if (screen != nullptr) {
            uint32_t child_cnt = lv_obj_get_child_cnt(screen);
            for (uint32_t i = 0; i < child_cnt; i++) {
                lv_obj_t* child = lv_obj_get_child(screen, i);
                if (child != nullptr && lv_obj_check_type(child, &lv_arc_class)) {
                    // 找到arc对象，可能是充电圆环，将其移到最前面
                    lv_obj_move_foreground(child);
                    lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
                    break;  // 只处理第一个arc对象（充电圆环）
                }
            }
        }
        
        lvgl_port_unlock();
        ESP_LOGI(TAG, "VideoPlayTask: LVGL锁已释放，首帧已显示");
    } else {
        ESP_LOGE(TAG, "VideoPlayTask: 获取LVGL锁失败");
        free(buf);
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // 首帧已显示，立即进入播放循环，不延迟
    ESP_LOGI(TAG, "VideoPlayTask: 进入视频播放循环，frames=%u, video_playing_=%d", 
             (unsigned)frames, self->video_playing_);
    uint32_t frame_counter = 1;  // 首帧已经显示，从1开始计数
    // 根据loop_group_标志决定是否循环播放
    if (self->loop_group_) {
        // 循环播放：idx 从 1 开始（首帧是 0，已经显示）
        idx = 1;
    } else {
        // 不循环：idx 从 1 开始（首帧是 0，已经显示）
        idx = 1;
    }
    
    while (self->video_playing_) {
        size_t off = group_base[g] + idx * frame_size;
        ESP_LOGD(TAG, "VideoPlayTask: 读取帧 %u, offset=%u", (unsigned)idx, (unsigned)off);
        if (flash.Read(off, buf, frame_size) != ESP_OK) {
            ESP_LOGE(TAG, "VideoPlayTask: 读取帧 %u 失败，offset=%u", (unsigned int)idx, (unsigned)off);
            break;
        }
        frame_counter++;
        if (frame_counter % 10 == 0) {  // 每10帧打印一次
            ESP_LOGI(TAG, "VideoPlayTask: 正在播放，帧 %u/%u, video_playing_=%d", 
                     (unsigned)idx, (unsigned)frames, self->video_playing_);
        }
        // 在读取Flash后让出CPU，避免长时间占用，影响音频处理
        if (frame_counter % 3 == 0) {  // 每3帧让出一次CPU
            taskYIELD();
        }
        // 使用非阻塞锁直接更新 LVGL，复用固定缓冲区 buf，避免内存泄漏
        // 视频任务优先级很低（0），不会阻塞音频处理
        int64_t lock_start_time = esp_timer_get_time();
        if (lvgl_port_lock(0)) {  // 非阻塞锁，立即返回，不等待
            int64_t lock_acquire_time = esp_timer_get_time() - lock_start_time;
            int64_t update_start_time = esp_timer_get_time();
            
            // 检查视频图像对象是否仍然有效（可能被配网模式等删除）
            if (self->video_img_ == nullptr || !lv_obj_is_valid(self->video_img_)) {
                ESP_LOGW(TAG, "VideoPlayTask: 视频图像对象已无效或被删除，停止播放");
                self->video_playing_ = false;
                free(buf);
                lvgl_port_unlock();
                vTaskDelete(nullptr);
                return;
            }
            
            self->video_img_dsc_.header.w = self->display_width_;
            self->video_img_dsc_.header.h = self->display_height_;
            self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            self->video_img_dsc_.data = buf;  // 直接使用固定缓冲区，避免内存分配
            self->video_img_dsc_.data_size = frame_size;
            
            // 只在每10帧更新一次充电圆环位置，减少LVGL操作
            bool update_charging_arc = (frame_counter % 10 == 0);
            
            // 先更新图像源（这是最耗时的操作）
            // 再次检查对象有效性（防止在更新过程中被删除）
            if (self->video_img_ == nullptr || !lv_obj_is_valid(self->video_img_)) {
                ESP_LOGW(TAG, "VideoPlayTask: 视频图像对象在更新前已无效，停止播放");
                self->video_playing_ = false;
                free(buf);
                lvgl_port_unlock();
                vTaskDelete(nullptr);
                return;
            }
            lv_img_set_src(self->video_img_, &self->video_img_dsc_);
            
            // 确保充电圆环始终显示在视频图像之上（减少频率）
            if (update_charging_arc) {
                // 如果缓存无效，重新查找
                if (self->cached_charging_arc_ == nullptr || 
                    !lv_obj_is_valid(self->cached_charging_arc_)) {
                    lv_obj_t* screen = lv_screen_active();
                    if (screen != nullptr) {
                        uint32_t child_cnt = lv_obj_get_child_cnt(screen);
                        for (uint32_t i = 0; i < child_cnt; i++) {
                            lv_obj_t* child = lv_obj_get_child(screen, i);
                            if (child != nullptr && lv_obj_check_type(child, &lv_arc_class)) {
                                self->cached_charging_arc_ = child;
                                break;
                            }
                        }
                    }
                }
                            
                // 更新充电圆环位置（只在需要时）
                if (self->cached_charging_arc_ != nullptr && 
                    lv_obj_is_valid(self->cached_charging_arc_)) {
                    lv_obj_move_foreground(self->cached_charging_arc_);
                    lv_obj_clear_flag(self->cached_charging_arc_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            
            lvgl_port_unlock();
            int64_t update_total_time = esp_timer_get_time() - update_start_time;
            int64_t total_time = esp_timer_get_time() - lock_start_time;
            
            if (frame_counter % 5 == 0) {  // 每5帧打印一次性能信息
                ESP_LOGI(TAG, "VideoPlayTask: 帧 %u 更新完成 - 锁获取: %lld us, 更新: %lld us, 总计: %lld us", 
                         (unsigned)idx, lock_acquire_time, update_total_time, total_time);
            }
            
            // 如果更新耗时超过10ms，增加额外延迟，避免占用过多CPU，影响音频处理
            if (update_total_time > 10000) {  // 10ms（降低阈值，更早让出CPU）
                ESP_LOGW(TAG, "VideoPlayTask: 帧更新耗时过长 (%lld us)，增加额外延迟", update_total_time);
                vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_ * 3));  // 三倍延迟，让出更多CPU时间
            } else if (update_total_time > 5000) {  // 5ms
                vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_ * 2));  // 双倍延迟
            } else {
                vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_));
            }
            // 主动让出CPU，确保音频任务有机会运行
            taskYIELD();
        } else {
            // 获取锁失败，跳过这一帧，不阻塞，让音频处理优先
            int64_t lock_fail_time = esp_timer_get_time() - lock_start_time;
            if (frame_counter % 5 == 0) {  // 每5帧打印一次
                ESP_LOGW(TAG, "VideoPlayTask: 获取LVGL锁失败（非阻塞），跳过帧 %u，耗时: %lld us", 
                         (unsigned)idx, lock_fail_time);
            }
            // 获取锁失败时也延迟，避免频繁尝试
            vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_));
            // 主动让出CPU，确保音频任务有机会运行
            taskYIELD();
        }
        
        // 根据loop_group_标志决定是否循环播放
        if (self->loop_group_) {
            // 循环播放同一组
            idx = (idx + 1) % frames;
        } else {
            // 不循环，播放完一组后触发回调
            idx++;
            if (idx >= frames) {
                ESP_LOGI(TAG, "VideoPlayTask: 组 %d 播放完成，共 %u 帧", g, frame_counter);
                break;  // 退出循环，触发回调
            }
        }
    }
    
    ESP_LOGI(TAG, "VideoPlayTask: 视频播放循环结束，video_playing_=%d, 总播放帧数=%u", 
             self->video_playing_, frame_counter);
    free(buf);
    self->video_playing_ = false;
    
    // 如果设置了播放完成回调，调用它
    if (self->on_group_finished_callback_ != nullptr) {
        ESP_LOGI(TAG, "VideoPlayTask: 调用播放完成回调，group_index=%d", self->video_group_index_);
        self->on_group_finished_callback_(self->on_group_finished_arg_, self->video_group_index_);
    }
    
    // 清理任务句柄，允许后续启动新的播放任务
    self->video_task_handle_ = nullptr;
    ESP_LOGI(TAG, "VideoPlayTask: 清理完成，任务即将删除");
    vTaskDelete(nullptr);
}

