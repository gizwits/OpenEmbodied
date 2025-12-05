#ifndef BOARDS_COMMON_PROTOCOL_H
#define BOARDS_COMMON_PROTOCOL_H

#include <cstdint>
#include <vector>
#include <cstring>

// 错误码定义（对外可见）
enum class ProtocolErrCode {
    SUCCESS = 0,
    CHECKSUM_ERROR = 2,  // 校验和错误
    TIMEOUT_ERROR = 3,   // 超时错误
    INVALID_PACKET = 4,  // 非法数据包
    CMD_UNSUPPORT = 5    // 不支持的命令
};

// 串口操作抽象接口（项目需自行实现）
class SerialInterface {
public:
    virtual ~SerialInterface() = default;
    // 发送数据
    virtual ProtocolErrCode send(const uint8_t* data, uint16_t len) = 0;
    // 接收数据（带超时）
    virtual ProtocolErrCode receive(uint8_t* data, uint16_t max_len, 
                                    uint16_t timeout_ms, uint16_t& recv_len) = 0;
};

// 协议配置参数（项目需自行填充）
struct ProtocolConfig {
    // 下面这几个版本字段在协议里都是「固定 8 字节」的 ASCII，不需要结尾 '\0'
    // 因此这里用显式的逐字节初始化，避免字符串字面量多出一个结尾 0 导致编译告警。
    uint8_t serial_prot_ver[8] = {'0','0','0','0','0','0','0','4'};  // 通用串口协议版本
    uint8_t biz_prot_ver[8]    = {'0','0','0','0','0','0','0','2'};  // 业务协议版本
    uint8_t hw_ver[8] = "V1.0.0";             // 硬件版本
    uint8_t sw_ver[8] = "V1.0.0";             // 软件版本
    uint8_t product_key[32] = {0};            // 产品标识码（机智云获取）
    uint8_t product_secret[32] = {0};         // 产品秘钥（机智云获取）
    uint16_t bind_expire = 0;                 // 可绑定失效时间（0=永久）
    uint8_t device_attr[8] = {0};             // 设备属性
    uint16_t timeout_ms = 200;                // 超时时间（ms）
    uint8_t retry_cnt = 3;                    // 重发次数
};

// 设备状态结构体（项目可扩展）
struct DeviceStatus {
    bool switch_status = false;        // 开关状态
    bool wakeup_word = false;          // 唤醒词状态
    uint8_t charge_status = 0;         // 充电状态
    uint8_t alert_tone_lang = 0;       // 提示音语言
    uint8_t chat_mode = 0;             // 对话模式
    uint8_t battery_percent = 0;       // 电池百分比
    uint8_t volume = 0;                // 音量
    int8_t rssi = -100;                // 信号强度（dBm）
    uint8_t temp_set = 0;              // 设置温度
    uint8_t temperature = 0;           // 当前温度
};

// WiFi模组工作状态（项目可扩展）
struct WifiModuleStatus {
    bool softap_en = false;        // SoftAP模式
    bool station_en = false;       // Station模式
    bool config_mode_en = false;   // 配置模式
    bool bind_mode_en = false;     // 绑定模式
    bool router_connected = false; // 连接路由器
    bool m2m_connected = false;    // 连接M2M服务器
    uint8_t rssi_level = 0;        // 信号强度等级
    bool app_online = false;       // APP在线
};

// 通用协议封装类（对外核心接口）
class Gizwits_Protocol {
public:
    // 构造：传入串口实现和配置
    Gizwits_Protocol(SerialInterface* serial, const ProtocolConfig& config);
    ~Gizwits_Protocol() = default;

    // 1. 回复设备信息（MCU→WiFi模组）
    ProtocolErrCode reply_device_info();
    
    // 1.1 查询设备信息（WiFi模组→MCU，发送0x01命令，接收0x02回复）
    ProtocolErrCode query_device_info(std::vector<uint8_t>& device_info);

    // 2. 控制设备（解析WiFi模组指令）
    ProtocolErrCode control_device(uint8_t action, uint16_t attr_flags, 
                                   const uint8_t* attr_vals, DeviceStatus& status);

    // 3. 主动上报设备状态（MCU→WiFi模组）
    ProtocolErrCode report_device_status(const DeviceStatus& status);

    // 4. 心跳回复
    ProtocolErrCode reply_heartbeat();

    // 5. 进入配置模式
    ProtocolErrCode enter_config_mode(uint8_t config_mode, 
                                      const char* ssid = "", 
                                      const char* password = "");

    // 6. 重置WiFi模组
    ProtocolErrCode reset_wifi_module();

    // 7. 解析WiFi模组状态
    ProtocolErrCode parse_wifi_status(const std::vector<uint8_t>& resp_payload, 
                                      WifiModuleStatus& module_status);

private:
    // 计算校验和
    uint8_t calc_checksum(const uint8_t* data, uint16_t len);
    // 转义/解转义
    void escape_data(const uint8_t* src, uint16_t src_len, std::vector<uint8_t>& dst);
    void unescape_data(const uint8_t* src, uint16_t src_len, std::vector<uint8_t>& dst);
    // 查找包头
    bool find_packet_header(const std::vector<uint8_t>& data, uint16_t& header_pos);
    // 构建基础数据包
    void build_base_packet(uint8_t cmd, uint8_t seq, uint16_t payload_len, std::vector<uint8_t>& packet);
    // 发送数据包（带重发）
    ProtocolErrCode send_packet(const std::vector<uint8_t>& payload, uint8_t cmd, std::vector<uint8_t>& response);

private:
    SerialInterface* serial_;       // 串口接口（项目实现）
    ProtocolConfig config_;         // 协议配置
    uint8_t packet_seq_ = 0;        // 包序号（0-255循环）
};

#endif // BOARDS_COMMON_PROTOCOL_H