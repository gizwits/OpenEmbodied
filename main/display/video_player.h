#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

// 视频播放器类，用于从外置 Flash 播放视频
// 完全独立的类，可以被多个板级项目复用
// 使用方式：直接创建 VideoPlayer 实例，然后调用 PlayVideoGroup() 即可
class VideoPlayer {
public:
    // 状态到组索引的映射函数类型
    using EmotionToGroupFunc = int(*)(const std::string&);

    // 构造函数
    // display_width: 显示宽度
    // display_height: 显示高度
    // frame_delay_ms: 每帧延迟时间（毫秒），默认 100ms（约 5 FPS）
    VideoPlayer(int display_width, int display_height, int frame_delay_ms = 100);
    
    ~VideoPlayer();

    // 设置状态到组索引的映射函数
    // 如果不设置，使用默认映射
    void SetEmotionToGroupFunc(EmotionToGroupFunc func) {
        emotion_to_group_func_ = func;
    }

    // 播放视频组（使用状态名称，如 "happy", "sad" 等）
    void PlayVideoGroup(const char* emotion);

    // 播放视频组（使用组索引）
    void PlayVideoGroupByIndex(int group_index);

    // 停止播放
    void StopPlayback();

    // 检查是否正在播放
    bool IsPlaying() const { return video_playing_; }

    // 获取当前播放的组索引
    int GetCurrentGroupIndex() const { return video_group_index_; }

    // 读取视频组数量
    int ReadVideoGroupCount();

private:
    // 播放任务
    static void VideoPlayTask(void* arg);
    
    // 启动播放
    void StartVideoPlayback();
    
    // 停止播放
    void StopVideoPlayback();

    // 默认的状态到组索引映射
    static int DefaultEmotionToGroup(const std::string& name);

    int display_width_;
    int display_height_;
    int frame_delay_ms_;
    
    TaskHandle_t video_task_handle_ = nullptr;
    bool video_playing_ = false;
    int video_group_index_ = 0;
    
    // LVGL 相关
    lv_obj_t* video_img_ = nullptr;
    lv_img_dsc_t video_img_dsc_{};
    
    // 状态映射函数
    EmotionToGroupFunc emotion_to_group_func_ = DefaultEmotionToGroup;
    
    // 重入保护
    TickType_t last_start_tick_ = 0;
    int last_started_group_ = -1;
};

