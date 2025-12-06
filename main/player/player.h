#pragma once

#include <functional>
#include <vector>
#include <string>
#include <memory>

class Player {
public:
    Player();
    ~Player();

    bool IsDownloading() const;
    // 设置数据包回调函数（使用移动语义避免数据复制）
    void setPacketCallback(std::function<void(std::vector<uint8_t>&&)> callback);
    // 设置队列状态查询回调（用于控制下载速度，返回当前队列大小）
    void setQueueSizeCallback(std::function<size_t()> callback);

    // 开始处理音频流
    bool processMP3Stream(const char* url);

    // 停止处理
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
