#ifndef __HTTP__
#define __HTTP__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Response_LEN 1*1024
extern char http_buf[Response_LEN];

typedef struct {
    char server[256]; // 服务器地址（主机名或域名）
    char port[8];     // 端口号（字符串形式）
    char path[256];   // 路径
} UrlComponents;

typedef bool (*ParseMessageCallback)(char *buffer, size_t length); 

int HttpParseUrl(const char *url, UrlComponents *components);
int HttpRequest(const char *server, const char *port, const char *content, size_t content_size, char *response_buffer, size_t buffer_size, ParseMessageCallback callback);

#ifdef __cplusplus
}
#endif
#endif