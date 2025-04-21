#pragma once

#include <cstdint>
#include <string>

namespace iot {
namespace protocol {

// Command constants
constexpr uint16_t CMD_VERSION_REPORT = 0x021c;
constexpr uint16_t CMD_VERSION_REPORT_RESP = 0x021d;

// Version information structure
struct VersionInfo {
    std::string subdev_id;         // 子设备ID
    std::string module_hw_ver;     // 模组硬件版本
    std::string module_sw_ver;     // 模组软件版本
    std::string mcu_hw_ver;        // MCU硬件版本
    std::string mcu_sw_ver;        // MCU软件版本
    std::string file_md5;          // 文件MD5
    std::string download_url;      // 固件下载链接
};

// Protocol data structure
struct ProtocolData {
    bool success;          // 解析是否成功
    uint16_t cmd;         // 命令字
    uint16_t flags;       // 命令标识位
    VersionInfo version_info;  // 版本信息
};

// Pack version report into buffer
// Returns number of bytes written or negative error code
int pack_version_report(uint8_t *buf, int buf_len, const VersionInfo &ver_info);

// Parse protocol data from buffer
// Returns parsed protocol data structure
ProtocolData parse_protocol_data(const uint8_t *buf, size_t len);

} // namespace protocol
} // namespace iot
