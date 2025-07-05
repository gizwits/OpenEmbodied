/* WebSocket Echo Server Example
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <esp_system.h>
 
#include "vb_ota.h"
#include "vb6824.h"
#include <esp_http_server.h>
#include "mdns.h"
#include "cJSON.h"
#include <esp_http_client.h>
#include <esp_wifi.h>
 
static const char *TAG = "ws_echo_server";
extern const char index_html_start[] asm("_binary_update_html_start");
static httpd_handle_t s_server = NULL;
static const size_t max_clients = 4;

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};
extern int vb6824_ota(const char* code, jl_ota_event_t evt_cb);
// Get all clients and send async message
static int wss_server_send_messages(uint8_t *data, uint32_t len, uint8_t type)
{
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = len;
    ws_pkt.type = type==1?HTTPD_WS_TYPE_TEXT:HTTPD_WS_TYPE_BINARY;

    if (!s_server) { // httpd might not have been created by now
        return -1;
    }
    size_t clients = max_clients;
    int    client_fds[max_clients];
    if (httpd_get_client_list(s_server, &clients, client_fds) == ESP_OK) {
        for (size_t i=0; i < clients; ++i) {
            int sock = client_fds[i];
            if (httpd_ws_get_fd_info(s_server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                // ESP_LOGI(TAG, "Active client (fd=%d) -> sending async message", sock);
                httpd_ws_send_frame_async(s_server, sock, &ws_pkt);
            }
        }
    } else {
        ESP_LOGE(TAG, "httpd_get_client_list failed!");
        return 1;
    }
    return 0;
}

static void vb_ota_evt_cb(jl_ota_evt_id evt, uint32_t data){
    switch (evt)
    {
    case JL_OTA_START:
        break;
    case JL_OTA_STOP:
        break;
    case JL_OTA_PROCESS:
        {
            char mss[64] = {0};
            sprintf(mss, "{\"status\":\"downloading\",\"progress\":%ld}", data);
            wss_server_send_messages((uint8_t*)mss, strlen(mss), 1);
            break;
        }
    case JL_OTA_FAIL: 
        wss_server_send_messages((uint8_t*)"{\"status\":\"fail\",\"reason\":\"升级失败,重试中\"}", strlen("{\"status\":\"fail\",\"reason\":\"升级失败,重试中\"}"), 1);
        break;
    case JL_OTA_SUCCESS:
        {
            char mss[64] = {0};
            sprintf(mss, "{\"status\":\"done\",\"word\":\"%s\"}", (char*)data);
            wss_server_send_messages((uint8_t*)mss, strlen(mss), 1);
            break;
        }
        break;
    default:
        break;
    }
    
}


static esp_err_t download_file_handler(httpd_req_t *req) {
    char url[128];
    char code[32];
    int ret = 0;
    // 从请求中获取URL
    httpd_req_get_url_query_str(req, url, sizeof(url));
    httpd_query_key_value(url, "id", code, sizeof(code));
    // 添加 CORS 响应头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // 允许所有来源访问
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS"); // 允许的请求方法
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type"); // 允许的请求头
    ret = vb6824_ota(code, vb_ota_evt_cb);
    char mss[128] = {0};
    sprintf(mss, "{\"status\":%s}", ret==1?"\"wait\"":"\"fail\", \"reason\":\"升级失败,请重启设备\"");
    wss_server_send_messages((uint8_t*)mss, strlen(mss), 1);
    httpd_resp_set_status(req, "200"); // 设置响应状态为200
    httpd_resp_set_type(req, "application/json"); // 设置响应类型为JSON
    httpd_resp_send(req, mss, strlen(mss)); // 发送响应内容
    return ESP_OK;
}

static esp_err_t _websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // First receive the full ws message
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }
    // If it was a PONG, update the keep-alive
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "Received PONG message");
        free(buf);
        return ESP_OK;

    // If it was a TEXT message, just echo it back
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING || ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGI(TAG, "Received packet with message: %s", ws_pkt.payload);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            // Response PONG packet to peer
            ESP_LOGI(TAG, "Got a WS PING frame, Replying PONG");
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            // Response CLOSE packet with no payload to peer
            ws_pkt.len = 0;
            ws_pkt.payload = NULL;
        }
        ret = httpd_ws_send_frame(req, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
        }
        ESP_LOGI(TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
                 httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
        free(buf);
        return ret;
    }
    free(buf);
    return ESP_OK;
}
 
// 提供主页的处理函数
static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // 允许所有来源访问
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS"); // 允许的请求方法
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type"); // 允许的请求头
    httpd_resp_send(req, index_html_start, strlen(index_html_start));
    return ESP_OK;
}

extern uint32_t check_code_legal(char *code);
static esp_err_t _check_legal(httpd_req_t *req){
    char url[128];
    char code[32];
    int ret = 0;
    // 从请求中获取URL
    httpd_req_get_url_query_str(req, url, sizeof(url));
    httpd_query_key_value(url, "id", code, sizeof(code));
    ret = check_code_legal(code);

    // 添加 CORS 响应头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // 允许所有来源访问
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS"); // 允许的请求方法
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type"); // 允许的请求头

    char mss[64] = {0};
    sprintf(mss, "{\"valid\":%d}", ret);
    httpd_resp_set_status(req, "200"); // 设置响应状态为200
    httpd_resp_set_type(req, "application/json"); // 设置响应类型为JSON
    httpd_resp_send(req, mss, strlen(mss)); // 发送响应内容
    return ESP_OK;
}

static esp_err_t options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0); // 发送空响应以结束请求
    return ESP_OK;
}

esp_err_t post_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;

    // 读取请求体
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
        if (ret < 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        remaining -= ret;
        buf[ret] = '\0'; // 确保字符串结束
        ESP_LOGI(TAG, "Received POST data: %s", buf);
    }
    // 解析 JSON 数据
    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON *id = cJSON_GetObjectItem(json, "url");
    if (id == NULL) {
        ESP_LOGE(TAG, "Missing 'url' field in JSON");
        cJSON_Delete(json);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char url[64] = {0};
    strcpy(url, id->valuestring);
    ESP_LOGI(TAG, "Received URL: %s", url);
    cJSON_Delete(json); // 释放 JSON 对象
    jl_set_ota_url((const char*)url); // 设置 OTA URL
    vb6824_ota("123123", vb_ota_evt_cb); // 启动 OTA 升级

    // 发送响应
    // 添加 CORS 响应头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // 允许所有来源访问
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS"); // 允许的请求方法
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type"); // 允许的请求头
    const char *response = "{\"valid\":1}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}

static const httpd_uri_t uri_post = {
    .uri       = "/dl_url", // 路由路径
    .method    = HTTP_POST, // HTTP 方法
    .handler   = post_handler, // 处理函数
    .user_ctx  = NULL
};

static const httpd_uri_t _options1_uri = {
    .uri       = "/dl_url",
    .method    = HTTP_OPTIONS,
    .handler   = options_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t download_code = {
    .uri       = "/code",
    .method    = HTTP_GET,
    .handler   = download_file_handler,
    .user_ctx  = NULL
};

// URI处理结构
static const httpd_uri_t main_index = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t _options2_uri = {
    .uri       = "/",
    .method    = HTTP_OPTIONS,
    .handler   = options_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t _check_uri = {
    .uri       = "/check",
    .method    = HTTP_GET,
    .handler   = _check_legal,
    .user_ctx  = NULL
};

static const httpd_uri_t _options_uri = {
    .uri       = "/check",
    .method    = HTTP_OPTIONS,
    .handler   = options_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = _websocket_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};
 
 void jl_ws_stop()
{
    httpd_stop(s_server);
    s_server = NULL;
}

int jl_ws_is_start(){
    return s_server==NULL?0:1;
}

int jl_ws_start(char *code)
{
    if (s_server!=NULL)
    {
        return 1;
    }

    // 获取设备的 IP 地址
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info);
    char ip[16] = {0};
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));

    // 发送请求到 test.cn/set?code={code}&ip={ip}
    char url[128] = {0};
    snprintf(url, sizeof(url), "http://tui.doit.am/configIp/ip.php?action=set&id=%s&ip=%s", code, ip);

    esp_http_client_config_t config1 = {
        .url = url,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config1);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Request to %s succeeded", url);
    } else {
        ESP_LOGE(TAG, "Request to %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);



    char host[12] = {0};
    sprintf(host, "aiota%s", code);
    // 初始化 mDNS
    mdns_init();
    mdns_hostname_set(host);  // 设置自定义的域名
    mdns_instance_name_set("ESP32 MDNS Example");
    mdns_txt_item_t serviceTxtData[] = {
        {"board", "ESP32"},
        {"version", "1.0"},
    };
    mdns_service_add("esp32", "_http", "_tcp", 80, serviceTxtData, sizeof(serviceTxtData) / sizeof(mdns_txt_item_t));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = max_clients;
    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&s_server, &config) == ESP_OK) {
        // Registering the ws handler
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(s_server, &ws);
        httpd_register_uri_handler(s_server, &_check_uri);
        httpd_register_uri_handler(s_server, &_options_uri);
        httpd_register_uri_handler(s_server, &main_index);
        httpd_register_uri_handler(s_server, &_options2_uri);
        httpd_register_uri_handler(s_server, &download_code);
        httpd_register_uri_handler(s_server, &uri_post);
        httpd_register_uri_handler(s_server, &_options1_uri);
        return 1;
    }
 
    ESP_LOGI(TAG, "Error starting server!");
    return 0;
}