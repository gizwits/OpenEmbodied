#include "config.h"
#include <esp_wifi.h>
#include "https_request.h"
#include "cJSON.h"
#include "esp_log.h"
#include "unit/gagent_md5.h"
#include "mqtt/mqtt.h"
#include "config.h"
#include "gsever.h"
#include "esp_random.h"
#include "audio_processor.h"

#ifdef DEBUG
    #define MALLOC(size) ({ \
        void *ptr = malloc(size); \
        ESP_LOGI(TAG, "MALLOC %p size %d at %s:%d", ptr, size, __FILE__, __LINE__); \
        ptr; \
    })
    
    #define FREE(ptr) do { \
        ESP_LOGI(TAG, "FREE %p at %s:%d", ptr, __FILE__, __LINE__); \
        free(ptr); \
    } while(0)
#endif

#define TAG "gsever"

#define SAFE_FREE(p) do { if(p) { FREE(p); p = NULL; } } while(0)

//////////////////////////////////////////////////////////////////////////////////////////////////////////
// 配网&请求MQTT
static const char *Onboarding_HDR = "POST %s HTTP/1.0\r\n"
                                "Host: %s:%s\r\n"
                                "X-Sign-Method: sha256\r\nX-Sign-Nonce: %s\r\nX-Sign-Token: %s\r\nX-Sign-ETag: %s\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %d\r\n"
                                "X-Trace-Id: %s\r\n"
                                "\r\n";

// 配网重置
static const char *Onboarding_BODY = "is_reset=%d&random_code=%s&lan_proto_ver=v5.0&user_id=%s";

// 请求MQTT
static const char *Provision_HDR = "GET %s HTTP/1.0\r\n"
                                "Host: %s:%s\r\n"
                                "X-Sign-Method: sha256\r\nX-Sign-Nonce: %s\r\nX-Sign-Token: %s\r\n"
                                "X-Trace-Id: %s\r\n"
                                "\r\n";


static const char * Provision_URL = "http://agent.gizwitsapi.com/v2/devices/%s/bootstrap";
static const char * Onboarding_URL = "http://agent.gizwitsapi.com/v2/devices/%s/network";



uint8_t szNonce[PASSCODE_LEN + 1];
static uint8_t pSZRandomCodeBase64[32+1] = {0};

void gatCreatNewPassCode(int8_t passCodeLen, uint8_t *pSZPassCode)
{
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

//16 进制转字符串
//flag =1 大写
//flag =0 小写
//dest 内存大小必须是scr 的2倍+1
void hexToStr( uint8_t *dest,uint8_t *src,int32_t srcLen,int8_t flag )
{
    int32_t i=0;
    for( i=0;i<srcLen;i++ )
    {
        if(flag)
        {
            sprintf( (void *)(dest+i*2),"%02X",src[i] );
        }

        else
        {
            sprintf( (void *)(dest+i*2),"%02x",src[i] );
        }
    }
}

// uint8_t *gatNetMACGet(void)
// {
//     uint8_t mac_hex[MAC_LEN] = {0};
//     static uint8_t device_mac[MAC_LEN + 1] = {0};
//     int ret;

//     // 获取设备MAC地址
//     ret = esp_wifi_get_mac(WIFI_IF_STA, mac_hex);
//     if (ret != 0) {
//         printf("get mac failed\n");
//         return NULL;
//     }

//     // 转换为hex字符串
//     hexToStr((uint8_t *) device_mac, (uint8_t *) mac_hex, NETIF_MAX_HWADDR_LEN, 0);
//     device_mac[MAC_LEN] = 0;
//     return device_mac;
// }

/**
 * 根据szNonce生成新的token
 * @param szNonce
 * @return
 */
const char *gatCreateToken(uint8_t *szNonce) {
    char input[32 * 4 + 1];
    char token_bin[CLOUD_TOKEN_BIN_LEN + 1];
    static char token[CLOUD_TOKEN_SZ_LEN + 1];
    uint8_t mac_hex[MAC_LEN + 1] = {0};
    uint8_t device_mac[MAC_LEN + 1] = {0};
    int ret;

    // 获取设备MAC地址
    // ret = esp_wifi_get_mac(WIFI_IF_STA, mac_hex);
    // if (ret != 0) {
    //     printf("get mac failed\n");
    //     return NULL;
    // }

    // 转换为hex字符串
    // hexToStr((uint8_t *) device_mac, (uint8_t *) mac_hex, NETIF_MAX_HWADDR_LEN, 0);
    get_mac_str(device_mac, false);
    device_mac[MAC_LEN] = 0;

    product_info_t *pInfo = get_product_info();
    memset(input, 0, sizeof(input));
    snprintf(input, sizeof(input) - 1, "%s,%s,%s,%s", pInfo->szPK, device_mac, pInfo->szAuthKey, szNonce);

    // 生成SHA256 token
    memset(token, 0, sizeof(token));
    ret = sha256_ret((uint8_t *) input, strlen(input), (uint8_t *) token_bin, 0);
    if (0 == ret) {
        hexToStr((uint8_t *) token, (uint8_t *) token_bin, 32, 0);
    }
    // hexdump("token_bin", token_bin, 32);
    return token;
}

uint8_t* genRandomCode( char *ssid, char* pwd, char *szPK, int sn)
{
    // uint8_t *pZRandomCode=NULL;
    // 初始化AES秘钥
    char AES_key[33];
    memset(AES_key, 0x30, 32);
    AES_key[32] = '\0';
    memcpy(AES_key, szPK, strlen(szPK));

    printf("genRandomCode \n ssid:%s\n pwd:%s\n szPK:%s\n sn:%08x\n AES_key:%s\n", ssid,pwd==NULL?"NULL":pwd,szPK,sn, AES_key);
    uint8_t szRandomCodeLen = 0;
    MD5_CTX ctx;
    uint8_t md5buf[128] = {0};
    uint8_t md5_calc[16] = {0};
    uint8_t szMD5Calc[33] = {0};
    uint8_t szRandomCode[128+1];
    // pZRandomCode = pSZRandomCodeBase64;
    if(pwd == NULL)
    {
        sprintf( (void *)md5buf,"%s",ssid);
    }else{
        sprintf( (void *)md5buf,"%s%s%08x",ssid,pwd,sn);
    }
    GAgent_MD5Init(&ctx);
    GAgent_MD5Update(&ctx, md5buf,strlen((const char *)md5buf));
    GAgent_MD5Final(&ctx, md5_calc);
    hexToStr( szMD5Calc,md5_calc,16,0 );
    printf("%s md5buf:%s ,md5_calc:%s .\n",__FUNCTION__,md5buf,szMD5Calc );
    aesInit();
    szRandomCodeLen = aesECB128Encrypt( (uint8_t*)AES_key,szRandomCode,md5_calc,32 );
    aesDestroy();
    printf("%s szRandomCodeLen=%d \n",__FUNCTION__,szRandomCodeLen);
    hexdump( "szRandomCode",szRandomCode,szRandomCodeLen );
    GAgent_MD5Init(&ctx);
    GAgent_MD5Update(&ctx, szRandomCode,szRandomCodeLen );
    GAgent_MD5Final(&ctx, md5_calc);
    hexdump("md5_calc",md5_calc,16 );
    hexToStr( (uint8_t*)pSZRandomCodeBase64,md5_calc,16,0 );
    printf("%s base64 :%s . \n",__FUNCTION__,pSZRandomCodeBase64 );
    return pSZRandomCodeBase64;
}

uint8_t* getRandomCode(void)
{
    product_info_t *pInfo = get_product_info();
    wifi_config_t wifi_config;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    printf("Connected SSID: %s\n", wifi_config.sta.ssid);
    printf("Connected password: %s\n", wifi_config.sta.password);
    genRandomCode(wifi_config.sta.ssid,wifi_config.sta.password,pInfo->szPK,0);
    return pSZRandomCodeBase64;
}

/**
 * 根据szNonce authkey body生成新的ETag
 * @param szNonce
 * @return
 */
const char *gatCreateETag(uint8_t *szNonce, uint8_t *body) 
{
    product_info_t *pInfo = get_product_info();
    char input[32 * 4 + 128 + 1]="";
    char ETag_bin[CLOUD_TOKEN_BIN_LEN + 1];
    static char ETag[CLOUD_TOKEN_SZ_LEN + 1];
    int8_t mac_hex[MAC_LEN + 1] = "\x00\x00\x00\x00\x00\x20";
    int8_t device_mac[MAC_LEN + 1] = "000000000020";
    int ret;
    
    memset(input, 0, sizeof(input));

    snprintf(input, sizeof(input) - 1, "%s,%s,%s", pInfo->szAuthKey, szNonce, body);
                
    // 生成SHA256 token
    memset(ETag, 0, sizeof(ETag));

    ret = sha256_ret((uint8_t *) input, strlen(input), (uint8_t *) ETag_bin, 0);
    if (0 == ret) {
        hexToStr((uint8_t *) ETag, (uint8_t *) ETag_bin, 32, 0);
    }
    printf("ETag: %s\n", ETag);
    printf("input: %s\n", input);
    hexdump("ETag_bin", ETag_bin, sizeof(ETag_bin));
    return ETag;
}

// 定义回调函数类型
typedef void (*mqtt_config_callback_t)(mqtt_config_t *config);


// 定义 Onboarding 回调函数类型
typedef void (*onboarding_callback_t)(onboarding_response_t *response);

// 添加静态变量存储回调函数
static mqtt_config_callback_t mqtt_config_cb = NULL;

int gatProvision_prase_cb(char* in_str, int in_len)
{
    product_info_t *pInfo = get_product_info();
    int status_code = 0;
    mqtt_config_t mqtt_config = {0};  // 创建临时配置结构体

    // 查找响应码 - 同时支持 HTTP/1.0 和 HTTP/1.1
    char *status_line = strstr(in_str, "HTTP/1.");
    if (status_line != NULL) {
        // 跳过 "HTTP/1.x "
        status_line = strchr(status_line, ' ');
        if (status_line != NULL) {
            status_line++; // 跳过空格
            status_code = atoi(status_line);
        }
    }

    ESP_LOGI(TAG, "Response header: %s", in_str);
    ESP_LOGI(TAG, "Status code: %d", status_code);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status_code);
        return -1; 
    }

    // 查找响应体
    char *body_start = strstr(in_str, "\r\n\r\n");
    if (body_start == NULL) {
        ESP_LOGE(TAG, "Invalid response format");
        return -2;
    }
    body_start += 4;

    ESP_LOGI(TAG, "Response body: %s", body_start);

    // 解析响应体
    const char *start = body_start;
    const char *end;

    while (1) {
        end = strchr(start, '&');
        if (end == NULL) {
            end = start + strlen(start);
        }

        char pair[256] = {0};
        int length = end - start;
        if (length >= sizeof(pair)) {
            ESP_LOGE(TAG, "Key-value pair too long");
            return -3;
        }

        strncpy(pair, start, length);
        pair[length] = '\0';

        char *separator = strchr(pair, '=');
        //separator 打印
        printf("separator: %s\n", separator);
        if (separator != NULL) {
            *separator = '\0';
            char *key = pair;
            char *value = separator + 1;

            // 将解析的值存入配置结构体
            if (strcmp(key, "product_key") == 0) {
                strncpy(mqtt_config.product_key, value, sizeof(mqtt_config.product_key) - 1);
                mqtt_config.product_key[sizeof(mqtt_config.product_key) - 1] = '\0';      // 确保字符串结束
            } else if (strcmp(key, "product_secret") == 0) {
                strncpy(mqtt_config.product_secret, value, sizeof(mqtt_config.product_secret) - 1);
                mqtt_config.product_secret[sizeof(mqtt_config.product_secret) - 1] = '\0';      // 确保字符串结束
            } else if (strcmp(key, "address") == 0) {
                strncpy(mqtt_config.mqtt_address, value, sizeof(mqtt_config.mqtt_address) - 1);
                mqtt_config.mqtt_address[sizeof(mqtt_config.mqtt_address) - 1] = '\0';      // 确保字符串结束
            } else if (strcmp(key, "port") == 0) {
                strncpy(mqtt_config.mqtt_port, value, sizeof(mqtt_config.mqtt_port) - 1);
                mqtt_config.mqtt_port[sizeof(mqtt_config.mqtt_port) - 1] = '\0';      // 确保字符串结束
            }
        }
        

        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }

    // 如果配置有效且回调函数存在，则调用回调
    if (strlen(mqtt_config.mqtt_address) > 0 && strlen(mqtt_config.mqtt_port) > 0) {
        if (mqtt_config_cb) {
            // 创建临时配置
            mqtt_config_t temp_config;
            memset(&temp_config, 0, sizeof(temp_config));
            
            // 安全拷贝
            strncpy(temp_config.mqtt_address, mqtt_config.mqtt_address, sizeof(temp_config.mqtt_address) - 1);
            strncpy(temp_config.mqtt_port, mqtt_config.mqtt_port, sizeof(temp_config.mqtt_port) - 1);
            
            // 调用回调
            mqtt_config_cb(&temp_config);
        }
    }

    return status_code;
}

int32_t gatProvision(mqtt_config_callback_t callback) {
    // 保存回调函数
    mqtt_config_cb = callback;
    
    char url[128] = {0};
    product_info_t *pInfo = get_product_info();
    int url_len = snprintf(url, sizeof(url) - 1, Provision_URL, pInfo->szDID);
    url[url_len] = 0;
    printf("%s url:%s\n", __FUNCTION__, url);

    // 解析 URL
    UrlComponents components = {0};
    if (HttpParseUrl(url, &components) != 0) {
        printf("Failed to parse url");
        return -2;
    }
    // 创建token
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    const char *token = gatCreateToken(szNonce);

    char *trace_id = get_trace_id();

    int absPathLen = strlen(Provision_HDR) +
                    strlen((const char *)(components.path)) +
                    strlen((const char *)(components.server)) +
                    strlen((const char *)(components.port)) +
                    strlen((const char *)(szNonce)) +
                    strlen((const char *)(token)) +
                    strlen((const char *)(trace_id)) + 1;
    char *absPath = MALLOC(absPathLen);
    if (NULL == absPath)
    {
        printf("MALLOC fail\n");
        return -1;
    }
    memset(absPath, 0, absPathLen);

    // 拼接URL
    int len = sprintf((void *)absPath, Provision_HDR, components.path, components.server, components.port, szNonce, token, trace_id);
    absPath[len] = '\0';

    // 打印生成的请求
    // printf("Generated Request:\n%s\n", absPath);

    // 返回http_parse_audio_agent的结果
    int ret = HttpRequest(components.server, components.port, absPath, strlen(absPath), http_buf, Response_LEN, gatProvision_prase_cb);

    // 释放内存
    FREE(absPath);
    return ret;
}

static uint8_t sOnboardingData[128];

/**
 * 执行Onboarding操作，获取连接参数
 * @return
 */
int32_t gatOnboarding(onboarding_callback_t callback) {
    char *absPath = NULL;
    char *body = NULL;
    mqtt_config_cb = callback;

    char url[128] = {0};  // 栈上分配的数组
    product_info_t *pInfo = get_product_info();

    int url_len = snprintf(url, sizeof(url) - 1, Onboarding_URL, pInfo->szDID);
    url[url_len] = 0;

    printf("gatOnboarding url: %s\n", url);

    const char *token, *ETag;

    // 解析 URL
    UrlComponents components = {0};
    if (HttpParseUrl(url, &components) != 0) {
        printf("Failed to parse url");
        return -2;  // 移除对url的释放
    }

    // 创建token
    gatCreatNewPassCode(PASSCODE_LEN, szNonce);
    token = gatCreateToken(szNonce);

    //内容拼接
    char uid[32] = {0};
    bool result = storage_load_uid(uid);
    // 打印 uid
    printf("uiduiduiduiduid: %s\n %d\n", uid, result);
    int len = snprintf(sOnboardingData, sizeof(sOnboardingData) - 1, Onboarding_BODY, 1, getRandomCode(),uid);
    sOnboardingData[len] = '\0';
    printf("sOnboardingData=\n%s \n",sOnboardingData);
    ETag = gatCreateETag(szNonce, sOnboardingData);

    char *trace_id = get_trace_id();

    int absPathLen = strlen(Onboarding_HDR) +
                    strlen((const char *)(components.path)) +
                    strlen((const char *)(components.server)) +
                    strlen((const char *)(components.port)) +
                    strlen((const char *)(szNonce)) +
                    strlen((const char *)(token)) + 
                    
                    strlen((const char *)(ETag)) +
                    strlen((const char *)(trace_id)) + len + 1;

    absPath = MALLOC(absPathLen);
    if (NULL == absPath)
    {
        printf("MALLOC fail\n");
        return -1;
    }
    memset(absPath, 0, absPathLen);

    int aLen = snprintf(absPath, absPathLen - 1, Onboarding_HDR, components.path, components.server, components.port, szNonce, token, ETag, len, trace_id);
    memcpy(absPath + aLen, sOnboardingData, len);
    absPath[aLen + len] = '\0';

    // 打印生成的请求
    // printf("Generated Request:\n%s\n", absPath);

    // 返回http_parse_audio_agent的结果
    int ret = HttpRequest(components.server, components.port, absPath, strlen(absPath), http_buf, Response_LEN, gatProvision_prase_cb);

    // 只释放动态分配的内存
    FREE(absPath);
    return ret;  // 移除对url的释放
}


#define HTTP_DEBUG
