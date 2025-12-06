#include "protocol.h"

// 构造函数
Gizwits_Protocol::Gizwits_Protocol(SerialInterface* serial, const ProtocolConfig& config)
    : serial_(serial), config_(config) {}

// 计算校验和
uint8_t Gizwits_Protocol::calc_checksum(const uint8_t* data, uint16_t len) {
    uint16_t sum = 0;
    for (uint16_t i = 0; i < len; i++) sum += data[i];
    return static_cast<uint8_t>(sum % 256);
}

// 转义处理（发送方）
// 注意：包头（0xFFFF）不转义，只转义非包头部分的0xFF
void Gizwits_Protocol::escape_data(const uint8_t* src, uint16_t src_len, std::vector<uint8_t>& dst) {
    dst.clear();
    if (src_len < 2) {
        // 数据太短，直接复制
        dst.assign(src, src + src_len);
        return;
    }
    
    // 包头（前2字节）直接复制，不转义
    dst.push_back(src[0]);
    dst.push_back(src[1]);
    
    // 从第3字节开始，对0xFF进行转义
    for (uint16_t i = 2; i < src_len; i++) {
        dst.push_back(src[i]);
        if (src[i] == 0xFF) {
            dst.push_back(0x55);  // 在非包头部分，0xFF后添加0x55
        }
    }
}

// 解转义处理（接收方）
// 注意：包头（0xFFFF）不转义，只解转义非包头部分的0xFF 0x55
void Gizwits_Protocol::unescape_data(const uint8_t* src, uint16_t src_len, std::vector<uint8_t>& dst) {
    dst.clear();
    if (src_len < 2) {
        // 数据太短，直接复制
        dst.assign(src, src + src_len);
        return;
    }
    
    // 包头（前2字节）直接复制，不解转义
    dst.push_back(src[0]);
    dst.push_back(src[1]);
    
    // 从第3字节开始，解转义0xFF 0x55
    for (uint16_t i = 2; i < src_len; i++) {
        if (src[i] == 0xFF && i + 1 < src_len && src[i+1] == 0x55) {
            // 在非包头部分，0xFF 0x55 还原为 0xFF
            dst.push_back(0xFF);
            i++;  // 跳过0x55
        } else {
            dst.push_back(src[i]);
        }
    }
}

// 查找包头（0xFFFF）
bool Gizwits_Protocol::find_packet_header(const std::vector<uint8_t>& data, uint16_t& header_pos) {
    if (data.size() < 2) {
        return false;
    }
    for (uint16_t i = 0; i + 1 < data.size(); ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xFF) {
            header_pos = i;
            return true;
        }
    }
    return false;
}

// 构建基础数据包
void Gizwits_Protocol::build_base_packet(uint8_t cmd, uint8_t seq, uint16_t payload_len, std::vector<uint8_t>& packet) {
    packet.clear();
    // 1. 包头（0xFFFF）
    packet.push_back(0xFF); packet.push_back(0xFF);
    // 2. 包长度（命令+序号+flags+payload+校验和）
    uint16_t pkg_len = 1 + 1 + 2 + payload_len + 1;
    packet.push_back(static_cast<uint8_t>((pkg_len >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>(pkg_len & 0xFF));
    // 3. 命令+序号+flags
    packet.push_back(cmd);
    packet.push_back(seq);
    packet.push_back(0x00); packet.push_back(0x00); // flags默认0
}

// 发送数据包（带重发）
ProtocolErrCode Gizwits_Protocol::send_packet(const std::vector<uint8_t>& payload, uint8_t cmd, std::vector<uint8_t>& response) {
    uint8_t current_seq = packet_seq_++;
    if (packet_seq_ > 255) packet_seq_ = 0;

    // 构建数据包
    std::vector<uint8_t> packet;
    build_base_packet(cmd, current_seq, payload.size(), packet);
    packet.insert(packet.end(), payload.begin(), payload.end());
    // 计算校验和
    uint8_t checksum = calc_checksum(&packet[2], packet.size() - 2);
    packet.push_back(checksum);
    // 转义
    std::vector<uint8_t> escaped_packet;
    escape_data(packet.data(), packet.size(), escaped_packet);

    // 重发逻辑
    for (uint8_t retry = 0; retry < config_.retry_cnt; retry++) {
        // 发送
        if (serial_->send(escaped_packet.data(), escaped_packet.size()) != ProtocolErrCode::SUCCESS) continue;
        // 接收
        uint8_t recv_buf[256] = {0};
        uint16_t recv_len = 0;
        if (serial_->receive(recv_buf, sizeof(recv_buf), config_.timeout_ms, recv_len) != ProtocolErrCode::SUCCESS) continue;
        // 解转义
        std::vector<uint8_t> unescaped_buf;
        unescape_data(recv_buf, recv_len, unescaped_buf);
        // 查找包头
        uint16_t header_pos = 0;
        if (!find_packet_header(unescaped_buf, header_pos)) continue;
        // 验证包长度
        uint16_t pkg_len = (unescaped_buf[header_pos + 2] << 8) | unescaped_buf[header_pos + 3];
        // 整帧长度应为：包头2 + 长度2 + pkg_len（cmd..checksum）
        if (unescaped_buf.size() < header_pos + 4 + pkg_len) continue;
        // 验证包序号
        if (unescaped_buf[header_pos + 5] != current_seq) continue;
        // 验证校验和
        uint16_t checksum_index = header_pos + 4 + pkg_len - 1; // cmd..checksum 的最后 1 字节
        if (checksum_index >= unescaped_buf.size()) continue;
        uint8_t resp_checksum = unescaped_buf[checksum_index];
        // 计算校验和：从长度字段开始，len2 + cmd..payload 的 (pkg_len + 1) 个字节
        uint8_t calc_check = calc_checksum(&unescaped_buf[header_pos + 2], static_cast<uint16_t>(pkg_len + 1));
        if (resp_checksum != calc_check) continue;
        // 提取响应
        uint16_t payload_start = header_pos + 8;
        uint16_t payload_len = pkg_len - 1 - (1 + 1 + 2);
        response.assign(unescaped_buf.begin() + payload_start, unescaped_buf.begin() + payload_start + payload_len);
        return ProtocolErrCode::SUCCESS;
    }
    return ProtocolErrCode::TIMEOUT_ERROR;
}

// 回复设备信息
ProtocolErrCode Gizwits_Protocol::reply_device_info() {
    std::vector<uint8_t> payload;
    // 协议版本/硬件版本等
    payload.insert(payload.end(), config_.serial_prot_ver, config_.serial_prot_ver + 8);
    payload.insert(payload.end(), config_.biz_prot_ver, config_.biz_prot_ver + 8);
    payload.insert(payload.end(), config_.hw_ver, config_.hw_ver + 8);
    payload.insert(payload.end(), config_.sw_ver, config_.sw_ver + 8);
    // 产品信息
    payload.insert(payload.end(), config_.product_key, config_.product_key + 32);
    payload.push_back(static_cast<uint8_t>((config_.bind_expire >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(config_.bind_expire & 0xFF));
    payload.insert(payload.end(), config_.device_attr, config_.device_attr + 8);
    payload.insert(payload.end(), config_.product_secret, config_.product_secret + 32);
    // DataLen默认0
    payload.push_back(0x00); payload.push_back(0x00);
    // 发送0x02命令
    std::vector<uint8_t> response;
    return send_packet(payload, 0x02, response);
}

// 查询设备信息（发送0x01命令，接收0x02回复）
ProtocolErrCode Gizwits_Protocol::query_device_info(std::vector<uint8_t>& device_info) {
    // 发送0x01命令（查询设备信息），payload为空
    std::vector<uint8_t> empty_payload;
    std::vector<uint8_t> response;
    ProtocolErrCode ret = send_packet(empty_payload, 0x01, response);
    if (ret == ProtocolErrCode::SUCCESS) {
        device_info = response;
    }
    return ret;
}

// 控制设备
ProtocolErrCode Gizwits_Protocol::control_device(uint8_t action, uint16_t attr_flags, const uint8_t* attr_vals, DeviceStatus& status) {
    if (action != 0x11) return ProtocolErrCode::CMD_UNSUPPORT;
    // 解析状态
    if (attr_flags & 0x0001) status.switch_status = (attr_vals[0] & 0x01);
    if (attr_flags & 0x0002) status.wakeup_word = (attr_vals[0] & 0x02);
    if (attr_flags & 0x0004) status.charge_status = (attr_vals[0] >> 2) & 0x03;
    if (attr_flags & 0x0008) status.alert_tone_lang = (attr_vals[0] >> 3) & 0x01;
    if (attr_flags & 0x0010) status.chat_mode = (attr_vals[0] >> 4) & 0x03;
    if (attr_flags & 0x0020) status.battery_percent = attr_vals[1];
    if (attr_flags & 0x0040) status.volume = attr_vals[2];
    if (attr_flags & 0x0100) status.temp_set = attr_vals[3];
    // 回复0x04命令
    std::vector<uint8_t> payload, response;
    return send_packet(payload, 0x04, response);
}

// 上报设备状态
ProtocolErrCode Gizwits_Protocol::report_device_status(const DeviceStatus& status) {
    std::vector<uint8_t> payload;
    payload.push_back(0x14); // action=上报状态
    // attr_flags
    uint16_t attr_flags = 0x0001 | 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0100 | 0x0200;
    payload.push_back(static_cast<uint8_t>((attr_flags >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(attr_flags & 0xFF));
    // 状态封装
    uint8_t dev_status_byte0 = 0;
    dev_status_byte0 |= status.switch_status ? 0x01 : 0x00;
    dev_status_byte0 |= status.wakeup_word ? 0x02 : 0x00;
    dev_status_byte0 |= (status.charge_status & 0x03) << 2;
    dev_status_byte0 |= (status.alert_tone_lang & 0x01) << 3;
    dev_status_byte0 |= (status.chat_mode & 0x03) << 4;
    payload.push_back(dev_status_byte0);
    payload.push_back(status.battery_percent);
    payload.push_back(status.volume);
    payload.push_back(static_cast<uint8_t>(status.rssi + 100));
    payload.push_back(status.temp_set);
    payload.push_back(status.temperature);
    // 发送0x05命令
    std::vector<uint8_t> response;
    return send_packet(payload, 0x05, response);
}

// 心跳回复
ProtocolErrCode Gizwits_Protocol::reply_heartbeat() {
    std::vector<uint8_t> payload, response;
    return send_packet(payload, 0x08, response);
}

// 进入配置模式
ProtocolErrCode Gizwits_Protocol::enter_config_mode(uint8_t config_mode, const char* ssid, const char* password) {
    std::vector<uint8_t> payload;
    payload.push_back(config_mode);
    if (config_mode == 4) {
        uint8_t ssid_len = strlen(ssid);
        uint8_t pwd_len = strlen(password);
        payload.push_back(ssid_len);
        payload.insert(payload.end(), (uint8_t*)ssid, (uint8_t*)ssid + ssid_len);
        payload.push_back(pwd_len);
        payload.insert(payload.end(), (uint8_t*)password, (uint8_t*)password + pwd_len);
        payload.push_back(0x00); // bssidlen
        payload.insert(payload.end(), 4, 0x00); // tzLen
        payload.push_back(0x00); // serverNameLen
    }
    std::vector<uint8_t> response;
    return send_packet(payload, 0x09, response);
}

// 重置WiFi模组
ProtocolErrCode Gizwits_Protocol::reset_wifi_module() {
    std::vector<uint8_t> payload, response;
    return send_packet(payload, 0x0b, response);
}

// 解析WiFi模组状态
ProtocolErrCode Gizwits_Protocol::parse_wifi_status(const std::vector<uint8_t>& resp_payload, WifiModuleStatus& module_status) {
    if (resp_payload.size() < 2) return ProtocolErrCode::INVALID_PACKET;
    uint16_t status = (resp_payload[0] << 8) | resp_payload[1];
    module_status.softap_en = (status & (1 << 0));
    module_status.station_en = (status & (1 << 1));
    module_status.config_mode_en = (status & (1 << 2));
    module_status.bind_mode_en = (status & (1 << 3));
    module_status.router_connected = (status & (1 << 4));
    module_status.m2m_connected = (status & (1 << 5));
    module_status.rssi_level = (status >> 8) & 0x07;
    module_status.app_online = (status & (1 << 11));
    return ProtocolErrCode::SUCCESS;
}