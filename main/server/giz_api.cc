#include "giz_api.h"
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cstring>
#include "auth.h"
#include <cstdio>
#include "mbedtls/md5.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "auth.h"
#include "settings.h"

#define TAG "GServer"

// 初始化静态成员变量
std::function<void(mqtt_config_t*)> GServer::mqtt_config_cb = nullptr;

GServer::GServer() {
    memset(szNonce, 0, sizeof(szNonce));
    memset(pSZRandomCodeBase64, 0, sizeof(pSZRandomCodeBase64));
    memset(sOnboardingData, 0, sizeof(sOnboardingData));
}

GServer::~GServer() {
    // Cleanup if needed
}

void GServer::gatCreatNewPassCode(int8_t passCodeLen, uint8_t *pSZPassCode) {
    // 使用硬件随机数生成器
    uint32_t random_seed = esp_random();
    
    memset(pSZPassCode, 0, passCodeLen);
    
    // 生成随机字符
    for(int8_t i = 0; i < passCodeLen; i++) {
        // 使用更好的随机源
        uint32_t rand_val = esp_random();
        
        // 更均匀的分布
        static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        pSZPassCode[i] = charset[rand_val % (sizeof(charset) - 1)];
    }
    
    pSZPassCode[passCodeLen] = '\0';
    ESP_LOGI(TAG, "Generated passcode length: %d", passCodeLen);
}

void GServer::hexToStr(uint8_t *dest, uint8_t *src, int32_t srcLen, int8_t flag) {
    int32_t i = 0;
    for(i = 0; i < srcLen; i++) {
        if(flag) {
            sprintf((char*)(dest + i * 2), "%02X", src[i]);
        } else {
            sprintf((char*)(dest + i * 2), "%02x", src[i]);
        }
    }
}

uint8_t* GServer::gatNetMACGet() {
    uint8_t mac_hex[MAC_LEN] = {0};
    static uint8_t device_mac[MAC_LEN + 1] = {0};
    int ret;

    // 获取设备MAC地址
    ret = esp_wifi_get_mac(WIFI_IF_STA, mac_hex);
    if (ret != 0) {
        ESP_LOGE(TAG, "get mac failed");
        return nullptr;
    }

    // 转换为hex字符串
    hexToStr(device_mac, mac_hex, NETIF_MAX_HWADDR_LEN, 0);
    device_mac[MAC_LEN] = 0;
    return device_mac;
}

const char* GServer::gatCreateToken(uint8_t *szNonce) {
    char input[32 * 4 + 1];
    char token_bin[CLOUD_TOKEN_BIN_LEN + 1];
    static char token[CLOUD_TOKEN_SZ_LEN + 1];
    uint8_t mac_hex[MAC_LEN + 1] = {0};
    uint8_t device_mac[MAC_LEN + 1] = {0};
    int ret;

    uint8_t* mac_ptr = GServer::gatNetMACGet();
    if (mac_ptr != nullptr) {
        memcpy(device_mac, mac_ptr, MAC_LEN + 1);
    }

    memset(input, 0, sizeof(input));
    std::string product_key = Auth::getInstance().getProductKey();
    std::string auth_key = Auth::getInstance().getAuthKey();
    snprintf(input, sizeof(input) - 1, "%s,%s,%s,%s", product_key.c_str(), device_mac, auth_key.c_str(), szNonce);

    ESP_LOGI(TAG, "token input: %s", input);
    // 生成SHA256 token
    memset(token, 0, sizeof(token));
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 means SHA256, not SHA224
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)input, strlen(input));
    mbedtls_sha256_finish(&sha256_ctx, (unsigned char*)token_bin);
    mbedtls_sha256_free(&sha256_ctx);
    
    hexToStr((uint8_t *)token, (uint8_t *)token_bin, 32, 0);
    return token;
}


const char* GServer::gatCreateLimitToken(uint8_t *szNonce) {
    char input[32 * 4 + 1];
    char token_bin[CLOUD_TOKEN_BIN_LEN + 1];
    static char token[CLOUD_TOKEN_SZ_LEN + 1];
    uint8_t mac_hex[MAC_LEN + 1] = {0};
    uint8_t device_mac[MAC_LEN + 1] = {0};
    int ret;

    uint8_t* mac_ptr = GServer::gatNetMACGet();
    if (mac_ptr != nullptr) {
        memcpy(device_mac, mac_ptr, MAC_LEN + 1);
    } else {
        ESP_LOGE(TAG, "Failed to get MAC address");
        return nullptr;
    }

    memset(input, 0, sizeof(input));
    std::string product_key = Auth::getInstance().getProductKey();
    std::string product_secret = Auth::getInstance().getProductSecret();
    std::string auth_key = Auth::getInstance().getAuthKey();
    snprintf(input, sizeof(input) - 1, "%s,%s,%s,%s", product_key.c_str(), device_mac, product_secret.c_str(), szNonce);

    ESP_LOGI(TAG, "token input: %s", input);
    // 生成SHA256 token
    memset(token, 0, sizeof(token));
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 means SHA256, not SHA224
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)input, strlen(input));
    mbedtls_sha256_finish(&sha256_ctx, (unsigned char*)token_bin);
    mbedtls_sha256_free(&sha256_ctx);
    
    hexToStr((uint8_t *)token, (uint8_t *)token_bin, 32, 0);
    return token;
}

uint8_t* GServer::genRandomCode(const char *ssid, const char* pwd, const char *szPK, int sn) {
    // 初始化AES秘钥
    char AES_key[33];
    memset(AES_key, 0x30, 32);
    AES_key[32] = '\0';
    memcpy(AES_key, szPK, strlen(szPK));

    ESP_LOGI(TAG, "genRandomCode \n ssid:%s\n pwd:%s\n szPK:%s\n sn:%08x\n AES_key:%s", 
             ssid, pwd == nullptr ? "NULL" : pwd, szPK, sn, AES_key);
    
    uint8_t szRandomCodeLen = 0;
    mbedtls_md5_context ctx;
    uint8_t md5buf[128] = {0};
    uint8_t md5_calc[16] = {0};
    uint8_t szMD5Calc[33] = {0};
    static uint8_t szRandomCode[128+1];

    if(pwd == nullptr) {
        sprintf((char*)md5buf, "%s", ssid);
    } else {
        sprintf((char*)md5buf, "%s%s%08x", ssid, pwd, sn);
    }
    
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, md5buf, strlen((const char *)md5buf));
    mbedtls_md5_finish(&ctx, md5_calc);
    mbedtls_md5_free(&ctx);
    hexToStr(szMD5Calc, md5_calc, 16, 0);
    
    ESP_LOGI(TAG, "%s md5buf:%s ,md5_calc:%s", __FUNCTION__, md5buf, szMD5Calc);
    
    // 使用 mbedtls AES 加密
    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_enc(&aes_ctx, (const unsigned char*)AES_key, 128);
    
    // 加密数据
    mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_ENCRYPT, md5_calc, szRandomCode);
    szRandomCodeLen = 16;  // AES-128 加密后固定为 16 字节
    
    mbedtls_aes_free(&aes_ctx);
    
    ESP_LOGI(TAG, "%s szRandomCodeLen=%d", __FUNCTION__, szRandomCodeLen);
    
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, szRandomCode, szRandomCodeLen);
    mbedtls_md5_finish(&ctx, md5_calc);
    mbedtls_md5_free(&ctx);
    static uint8_t pSZRandomCodeBase64[32+1];
    hexToStr(pSZRandomCodeBase64, md5_calc, 16, 0);
    
    ESP_LOGI(TAG, "%s base64 :%s", __FUNCTION__, pSZRandomCodeBase64);
    return pSZRandomCodeBase64;
}

uint8_t* GServer::getRandomCode() {
    wifi_config_t wifi_config;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    ESP_LOGI(TAG, "Connected SSID: %s", wifi_config.sta.ssid);
    ESP_LOGI(TAG, "Connected password: %s", wifi_config.sta.password);
    std::string product_key = Auth::getInstance().getProductKey();
    return genRandomCode((const char*)wifi_config.sta.ssid, (const char*)wifi_config.sta.password, product_key.c_str(), 0);
}

const char* GServer::gatCreateETag(uint8_t *szNonce, uint8_t *body) {
    char input[32 * 4 + 128 + 1] = "";
    char ETag_bin[CLOUD_TOKEN_BIN_LEN + 1];
    static char ETag[CLOUD_TOKEN_SZ_LEN + 1];
    int8_t mac_hex[MAC_LEN + 1] = "\x00\x00\x00\x00\x00\x20";
    int8_t device_mac[MAC_LEN + 1] = "000000000020";
    int ret;
    
    memset(input, 0, sizeof(input));
    std::string auth_key = Auth::getInstance().getAuthKey();
    snprintf(input, sizeof(input) - 1, "%s,%s,%s", auth_key.c_str(), szNonce, body);
                
    // 生成SHA256 token
    memset(ETag, 0, sizeof(ETag));
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 means SHA256, not SHA224
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)input, strlen(input));
    mbedtls_sha256_finish(&sha256_ctx, (unsigned char*)ETag_bin);
    mbedtls_sha256_free(&sha256_ctx);
    
    hexToStr((uint8_t *)ETag, (uint8_t *)ETag_bin, 32, 0);
    
    ESP_LOGI(TAG, "ETag: %s", ETag);
    ESP_LOGI(TAG, "input: %s", input);
    return ETag;
}

int GServer::getProvision_prase_cb(const char* in_str, int in_len) {
    mqtt_config_t mqtt_config = {0};
    char* params = strdup(in_str);
    char* saveptr = nullptr;
    char* token = strtok_r(params, "&", &saveptr);

    while (token != nullptr) {
        if (strncmp(token, "address=", 8) == 0) {
            strncpy(mqtt_config.mqtt_address, token + 8, sizeof(mqtt_config.mqtt_address) - 1);
        } else if (strncmp(token, "port=", 5) == 0) {
            strncpy(mqtt_config.mqtt_port, token + 5, sizeof(mqtt_config.mqtt_port) - 1);
        } else if (strncmp(token, "restart_time=", 13) == 0) {
            // mqtt_config.restart_time = atoi(token + 13);
        } else if (strncmp(token, "tz_offset=", 10) == 0) {
            // mqtt_config.tz_offset = atoi(token + 10);
        } else if (strncmp(token, "device_id=", 10) == 0) {
            strncpy(mqtt_config.device_id, token + 10, sizeof(mqtt_config.device_id) - 1);
        }
        token = strtok_r(nullptr, "&", &saveptr);
    }

    free(params);

    ESP_LOGI(TAG, "Parsed MQTT config: address=%s, port=%s",
             mqtt_config.mqtt_address, mqtt_config.mqtt_port);

    if (mqtt_config.mqtt_address[0] == '\0' || mqtt_config.mqtt_port[0] == '\0') {
        ESP_LOGE(TAG, "Invalid MQTT config: missing address or port");
        return -1;
    }

    if (mqtt_config_cb) {
        mqtt_config_cb(&mqtt_config);
    }

    return 0;
}

int32_t GServer::getProvision(std::function<void(mqtt_config_t*)> callback) {
    mqtt_config_cb = callback;
    std::string did = Auth::getInstance().getDeviceId();
    std::string url = "http://agent.gizwitsapi.com/v2/devices/" + did + "/bootstrap";
    ESP_LOGI(TAG, "Provision URL: %s", url.c_str());

    // 创建token
    static uint8_t szNonce[PASSCODE_LEN + 1];
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    const char *token = gatCreateToken(szNonce);

    // 使用Board的HTTP客户端
    auto& board = Board::GetInstance();
    auto http = board.CreateHttp();
    
    // 设置请求头
    http->SetHeader("X-Sign-Method", "sha256");
    http->SetHeader("X-Sign-Nonce", (const char*)szNonce);
    http->SetHeader("X-Sign-Token", token);
    http->SetHeader("X-Trace-Id", get_trace_id());

    // 发送GET请求
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        delete http;
        return -1;
    }

    std::string response = http->GetBody();
    delete http;

    return getProvision_prase_cb(response.c_str(), response.length());
}


int32_t GServer::getLimitProvision(std::function<void(mqtt_config_t*)> callback) {
    mqtt_config_cb = callback;
    std::string did = Auth::getInstance().getDeviceId();
    std::string product_key = Auth::getInstance().getProductKey();
    std::string mac = (char*)GServer::gatNetMACGet();

    std::string url = "http://agent.gizwitsapi.com/v2/products/" + product_key + "/devices/" + mac + "/bootstrap";
    ESP_LOGI(TAG, "Provision URL: %s", url.c_str());

    // 创建token
    static uint8_t szNonce[PASSCODE_LEN + 1];
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    const char *token = gatCreateLimitToken(szNonce);

    // 使用Board的HTTP客户端
    auto& board = Board::GetInstance();
    auto http = board.CreateHttp();
    
    // 设置请求头
    http->SetHeader("X-Sign-Method", "sha256");
    http->SetHeader("X-Sign-Nonce", (const char*)szNonce);
    http->SetHeader("X-Sign-Token", token);
    http->SetHeader("X-Trace-Id", get_trace_id());

    // 发送GET请求
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        delete http;
        return -1;
    }

    std::string response = http->GetBody();
    delete http;

    return getProvision_prase_cb(response.c_str(), response.length());
}

int32_t GServer::activationDevice(std::function<void(mqtt_config_t*)> callback) {
    mqtt_config_cb = callback;
    std::string did = Auth::getInstance().getDeviceId();
    std::string url = "http://agent.gizwitsapi.com/v2/devices/" + did + "/network";
    ESP_LOGI(TAG, "Onboarding URL: %s", url.c_str());

    // 创建token
    static uint8_t szNonce[PASSCODE_LEN + 1];
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    const char *token = gatCreateToken(szNonce);

    // 准备请求体
    Settings settings("wifi", true);
    std::string uid = settings.GetString("uid", "");
    ESP_LOGI(TAG, "UID: %s", uid.c_str());
    
    static uint8_t sOnboardingData[128];
    int len = snprintf((char*)sOnboardingData, sizeof(sOnboardingData) - 1, 
                      "is_reset=1&random_code=%s&lan_proto_ver=v5.0&user_id=%s", 
                      getRandomCode(), uid.c_str());
    sOnboardingData[len] = '\0';
    
    const char *ETag = gatCreateETag(szNonce, sOnboardingData);

    // 使用Board的HTTP客户端
    auto& board = Board::GetInstance();
    auto http = board.CreateHttp();
    
    // 设置请求头
    http->SetHeader("X-Sign-Method", "sha256");
    http->SetHeader("X-Sign-Nonce", (const char*)szNonce);
    http->SetHeader("X-Sign-Token", token);
    http->SetHeader("X-Sign-ETag", ETag);
    http->SetHeader("X-Trace-Id", get_trace_id());
    http->SetHeader("Content-Type", "text/plain");

    // 发送POST请求
    if (!http->Open("POST", url, (const char*)sOnboardingData)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        delete http;
        return -1;
    }

    std::string response = http->GetBody();
    delete http;
    ESP_LOGI(TAG, "response: %s", response.c_str());

    return getProvision_prase_cb(response.c_str(), response.length());
}

char* GServer::get_trace_id() {
    static char trace_id[33];
    uint8_t random_bytes[16];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    for (int i = 0; i < 16; i++) {
        sprintf(trace_id + i * 2, "%02x", random_bytes[i]);
    }
    trace_id[32] = '\0';
    return trace_id;
} 