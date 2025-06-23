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
    // 设置数据包回调函数
    void setPacketCallback(std::function<void(const std::vector<uint8_t>&)> callback);

    // 开始处理音频流
    bool processMP3Stream(const char* url);

    // 停止处理
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
