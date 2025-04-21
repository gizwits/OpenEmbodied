#ifndef _GSERVER_HPP_
#define _GSERVER_HPP_

#include <string>
#include <functional>
#include "board.h"
#include "http.h"

#define PASSCODE_LEN 10
#define MAC_LEN 16
#define CLOUD_TOKEN_BIN_LEN 32
#define CLOUD_TOKEN_SZ_LEN 64
#define Response_LEN 4096
#define NETIF_MAX_HWADDR_LEN    6U


struct product_info_t {
    char szPK[32];
    char szDID[32];
    char szAuthKey[32];
};

struct mqtt_config_t {
    char product_key[32];
    char product_secret[32];
    char mqtt_address[64];
    char mqtt_port[8];
};

struct onboarding_response_t {
    bool success;
    std::string message;
};

class GServer {
public:
    GServer();
    ~GServer();

    static GServer& getInstance() {
        static GServer instance;
        return instance;
    }

    // 生成随机码
    static void gatCreatNewPassCode(int8_t passCodeLen, uint8_t *pSZPassCode);
    
    // 获取设备MAC地址
    static uint8_t* gatNetMACGet();
    
    // 创建token
    static const char* gatCreateToken(uint8_t *szNonce);
    
    // 生成随机码
    static uint8_t* genRandomCode(const char *ssid, const char* pwd, const char *szPK, int sn);
    
    // 获取随机码
    static uint8_t* getRandomCode();
    
    // 创建ETag
    static const char* gatCreateETag(uint8_t *szNonce, uint8_t *body);
    
    // 解析MQTT配置
    static int gatProvision_prase_cb(const char* in_str, int in_len);
    
    // 获取MQTT配置
    static int32_t gatProvision(std::function<void(mqtt_config_t*)> callback);
    
    // 执行Onboarding
    static int32_t gatOnboarding(std::function<void(onboarding_response_t*)> callback);

private:
    // 16进制转字符串
    static void hexToStr(uint8_t *dest, uint8_t *src, int32_t srcLen, int8_t flag);
    
    // 获取trace_id
    static char* get_trace_id();

    // 静态成员变量
    static std::function<void(mqtt_config_t*)> mqtt_config_cb;
    static std::function<void(onboarding_response_t*)> onboarding_cb;

    // 非静态成员变量
    uint8_t szNonce[PASSCODE_LEN];
    uint8_t pSZRandomCodeBase64[PASSCODE_LEN];
    uint8_t sOnboardingData[Response_LEN];
};

#endif // _GSERVER_HPP_ 