#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "protocol_examples_common.h"
#include "esp_sntp.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_tls.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "config.h"
#include "cJSON.h"


#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <esp_log.h>
#include "https_request.h"
#include "audio_processor.h"

static const char *TAG = "https";

#define Response_LEN 1*1024
char http_buf[Response_LEN];

// 解析 URL 的函数
static int ParseUrl(const char *url, UrlComponents *components) {
    char buffer[512];
    char *ptr;
    char *token;

    // 初始化结构体
    memset(components, 0, sizeof(UrlComponents));
    strcpy(components->port, "-1"); // 默认端口号为 -1，表示未指定

    // 检查 URL 是否以 "http://" 或 "https://" 开头
    if (strncmp(url, "http://", 7) == 0) {
        strncpy(buffer, url + 7, sizeof(buffer) - 1); // 跳过 "http://"
        strcpy(components->port, "80");              // HTTP 默认端口
    } else if (strncmp(url, "https://", 8) == 0) {
        strncpy(buffer, url + 8, sizeof(buffer) - 1); // 跳过 "https://"
        strcpy(components->port, "443");             // HTTPS 默认端口
    } else {
        strncpy(buffer, url, sizeof(buffer) - 1); // 无协议前缀
    }
    buffer[sizeof(buffer) - 1] = '\0';

    // 提取服务器地址和端口号
    ptr = strchr(buffer, '/');
    if (ptr) {
        *ptr = '\0'; // 将路径部分截断
    }

    token = strchr(buffer, ':');
    if (token) {
        *token = '\0'; // 将端口部分截断
        strncpy(components->server, buffer, sizeof(components->server) - 1);
        components->server[sizeof(components->server) - 1] = '\0';
        strncpy(components->port, token + 1, sizeof(components->port) - 1);
        components->port[sizeof(components->port) - 1] = '\0';
    } else {
        strncpy(components->server, buffer, sizeof(components->server) - 1);
        components->server[sizeof(components->server) - 1] = '\0';
    }

    // 提取路径
    if (ptr) {
        // 确保路径以 '/' 开头
        if (*(ptr + 1) != '\0') {
            snprintf(components->path, sizeof(components->path), "/%s", ptr + 1);
        } else {
            strcpy(components->path, "/"); // 如果路径为空，设置为 "/"
        }
    } else {
        strcpy(components->path, "/"); // 默认路径为 "/"
    }

    return 0; // 成功
}

int HttpParseUrl(const char *url, UrlComponents *components) {
    if (components == NULL) {
        printf("Invalid components pointer\n");
        return -1;
    }

    if (ParseUrl(url, components) == 0) {
        printf("Server: %s\n", components->server);
        printf("Port: %s\n", components->port);
        printf("Path: %s\n", components->path);
    } else {
        
        printf("Failed to parse URL\n");
        return -2;
    }
    return 0;
}

int HttpRequest(const char *server, const char *port, const char *content, size_t content_size, char *response_buffer, size_t buffer_size, ParseMessageCallback callback) {
    if (!server || !port || !content || !content_size || !response_buffer || !buffer_size) {
        ESP_LOGE(TAG, "Invalid input parameters");
        return -1;
    }

    ESP_LOGI(TAG, "Initiating HTTP request");

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    int dns_retry = 0;
    const int max_dns_retries = 3;  // 最大DNS重试次数

    // 打印 server port content
    printf("server: %s\n", server);
    printf("port: %s\n", port);
    printf("content: %s\n", content);


    while(1) {
        printf("start get addrinfo\n");
        int err = getaddrinfo(server, port , &hints, &res);
        printf("get addrinfo done\n");

        if(err != 0 || res == NULL) {
            dns_retry++;
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p (attempt %d/%d)", 
                     err, res, dns_retry, max_dns_retries);
            
            if (dns_retry >= max_dns_retries) {
                ESP_LOGE(TAG, "DNS lookup failed after %d attempts", max_dns_retries);
                audio_tone_play(1, 0, "spiffs://spiffs/network_error_need_reset.mp3");
                return ESP_ERR_NOT_FOUND;
            }
            
            // 每次重试增加延迟时间
            vTaskDelay(pdMS_TO_TICKS(1000 * dns_retry));
            continue;
        }

        // DNS查找成功，重置重试计数
        dns_retry = 0;
        /* Code to print the resolved IP.
           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, content, content_size) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");

        /* Read HTTP response */
        memset(response_buffer, 0, buffer_size);
        do {
            r = read(s, response_buffer, buffer_size - 1);
        } while(r > 0);

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
        close(s);
        ESP_LOGI(TAG, "Starting again!");
        printf("Response: %s\n", response_buffer);
        break;
    }

    if (callback) {
        return callback(response_buffer, buffer_size);
    }
    return 0;
}
