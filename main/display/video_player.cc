#include "video_player.h"
#include "w25q64_flash.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <cstring>

static const char* TAG = "VideoPlayer";

int VideoPlayer::DefaultEmotionToGroup(const std::string& name) {
    // 状态映射，和 SetEmotion 支持的状态保持一致
    // 将相似的状态映射到同一个视频组
    
    // 组 0: 开心相关
    if (name == "happy") return 0;
    if (name == "laughing") return 0;
    if (name == "funny") return 0;
    if (name == "cool") return 0;
    if (name == "relaxed") return 0;
    if (name == "confident") return 0;
    
    // 组 1: 中性/待机
    if (name == "neutral") return 1;
    
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
    
    // 默认返回开心组
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
    if (emotion == nullptr) {
        ESP_LOGE(TAG, "PlayVideoGroup: emotion is nullptr");
        return;
    }
    int group_index = emotion_to_group_func_(std::string(emotion));
    PlayVideoGroupByIndex(group_index);
    ESP_LOGI(TAG, "PlayVideoGroup: emotion=%s -> group_index=%d", emotion, group_index);
}

void VideoPlayer::PlayVideoGroupByIndex(int group_index) {
    int cnt = ReadVideoGroupCount();
    if (cnt <= 0) {
        ESP_LOGE(TAG, "No video groups available");
        return;
    }
    if (group_index < 0 || group_index >= cnt) {
        ESP_LOGE(TAG, "Invalid group index: %d (max: %d)", group_index, cnt - 1);
        return;
    }
    video_group_index_ = group_index;
    if (video_playing_) StopPlayback();
    StartVideoPlayback();
    ESP_LOGI(TAG, "PlayVideoGroupByIndex: group_index=%d", group_index);
}

void VideoPlayer::StartVideoPlayback() {
    // 重入保护：同一组1秒内的重复启动忽略
    TickType_t now = xTaskGetTickCount();
    if (last_started_group_ == video_group_index_ &&
        (now - last_start_tick_) < pdMS_TO_TICKS(1000)) {
        return;
    }
    // 若已有任务在跑，先停止并等待退出
    if (video_task_handle_ != nullptr) {
        video_playing_ = false;
        for (int i = 0; i < 50 && video_task_handle_ != nullptr; ++i) { // 最多等 500ms
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        video_task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "StartVideoPlayback group=%d", video_group_index_);
    video_playing_ = true;
    // 降低优先级，避免阻塞音频任务
    xTaskCreate(VideoPlayTask, "video_play", 4096, this, 1, &video_task_handle_);
    last_started_group_ = video_group_index_;
    last_start_tick_ = now;
}

void VideoPlayer::StopVideoPlayback() {
    if (!video_playing_ && video_task_handle_ == nullptr) return;
    video_playing_ = false;
    // 等待任务自删除清理句柄
    for (int i = 0; i < 50 && video_task_handle_ != nullptr; ++i) { // 最多等 500ms
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    video_task_handle_ = nullptr;
}

void VideoPlayer::StopPlayback() {
    StopVideoPlayback();
    // 隐藏视频图像，避免与Display动画重叠
    if (video_img_ != nullptr) {
        if (lvgl_port_lock(1000)) {
            // 隐藏视频图像
            lv_obj_add_flag(video_img_, LV_OBJ_FLAG_HIDDEN);
            // 将视频图像移到后面，确保Display对象显示在最前面
            lv_obj_move_background(video_img_);
            // 清空图像数据，避免显示最后一帧
            video_img_dsc_.data = nullptr;
            video_img_dsc_.data_size = 0;
            lv_img_set_src(video_img_, &video_img_dsc_);
            lvgl_port_unlock();
        }
    }
}

void VideoPlayer::VideoPlayTask(void* arg) {
    auto* self = static_cast<VideoPlayer*>(arg);
    
    // 使用外置 Flash
    auto& flash = W25Q64Flash::GetInstance();
    if (!flash.IsInitialized()) {
        ESP_LOGE(TAG, "External flash not initialized");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // Read header: 1 byte count + N*4 bytes frame counts
    uint8_t group_count = 0;
    if (flash.Read(0, &group_count, 1) != ESP_OK || group_count == 0) {
        ESP_LOGE(TAG, "invalid video header");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
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
    if (g < 0 || g >= group_count) g = 0;
    uint32_t frames = frame_counts[g];
    ESP_LOGI(TAG, "Video header: groups=%u, frame_size=%u, data_offset=%u, play_group=%d, frames_in_group=%u, group_base=%u",
             (unsigned)group_count, (unsigned)frame_size, (unsigned)data_offset, g, (unsigned)frames, (unsigned)group_base[g]);
    
    if (frames == 0) {
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // allocate frame buffer
    // Prefer DMA-capable internal memory for SPI DMA
    uint8_t* buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(frame_size, MALLOC_CAP_INTERNAL);
    if (!buf) buf = (uint8_t*)malloc(frame_size);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for frame buffer");
        self->video_playing_ = false;
        vTaskDelete(nullptr);
        return;
    }
    
    // 先读取首帧并设置到图像，再显示，避免切组时先显示空白导致黑屏扫描
    uint32_t idx = 0;
    {
        size_t off0 = group_base[g] + idx * frame_size;
        if (flash.Read(off0, buf, frame_size) != ESP_OK) {
            ESP_LOGE(TAG, "read first frame %u failed", (unsigned int)idx);
            free(buf);
            self->video_playing_ = false;
            vTaskDelete(nullptr);
            return;
        }
        if (lvgl_port_lock(1000)) {
            if (self->video_img_ == nullptr) {
                self->video_img_ = lv_image_create(lv_screen_active());
                lv_obj_set_size(self->video_img_, self->display_width_, self->display_height_);
                // 使用顶部对齐并向上偏移15像素
                lv_obj_align(self->video_img_, LV_ALIGN_TOP_MID, 0, -15);
            }
            self->video_img_dsc_.header.w = self->display_width_;
            self->video_img_dsc_.header.h = self->display_height_;
            self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            self->video_img_dsc_.data = buf;
            self->video_img_dsc_.data_size = frame_size;
            lv_img_set_src(self->video_img_, &self->video_img_dsc_);
            lv_obj_move_foreground(self->video_img_);
            lv_obj_clear_flag(self->video_img_, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_));
        idx = (idx + 1) % frames;
    }
    
    while (self->video_playing_) {
        size_t off = group_base[g] + idx * frame_size;
        if (flash.Read(off, buf, frame_size) != ESP_OK) {
            ESP_LOGE(TAG, "read frame %u failed", (unsigned int)idx);
            break;
        }
        // 每帧短锁，更新 LVGL 图像（减少锁定时间，避免阻塞音频任务）
        if (lvgl_port_lock(20)) {  // 从50ms减少到20ms，更快释放锁
            self->video_img_dsc_.header.w = self->display_width_;
            self->video_img_dsc_.header.h = self->display_height_;
            self->video_img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            self->video_img_dsc_.data = buf;
            self->video_img_dsc_.data_size = frame_size;
            lv_img_set_src(self->video_img_, &self->video_img_dsc_);
            lvgl_port_unlock();
        }
        if ((idx % 10) == 0) {
            ESP_LOGI(TAG, "Playing group=%d idx=%u/%u off=%u", g, (unsigned)idx, (unsigned)frames, (unsigned)off);
        }
        vTaskDelay(pdMS_TO_TICKS(self->frame_delay_ms_));
        idx = (idx + 1) % frames; // 当前分组循环播放，直到按键切换
    }
    
    free(buf);
    self->video_playing_ = false;
    // 清理任务句柄，允许后续启动新的播放任务
    self->video_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

