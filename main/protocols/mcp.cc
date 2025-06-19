#include <stdbool.h>
#include <string_view>
#include <string.h>
#include "cJSON.h"
#include "mcp.h"
#include "esp_timer.h"
#include <esp_log.h>
#include "esp_err.h"
#include "iot/thing_manager.h"
#include "application.h"


const char* CozeMCPParser::TAG = "VolcRTCApp";

CozeMCPParser& CozeMCPParser::getInstance() {
    static CozeMCPParser instance;
    return instance;
}

void CozeMCPParser::handle_mcp(std::string_view data) {
    // 解析JSON以获取tool_call_id
    ESP_LOGI(TAG, "handle_mcp: %.*s", static_cast<int>(data.size()), data.data());
    char event_id[32];
    snprintf(event_id, sizeof(event_id), "%lld", esp_timer_get_time());
    cJSON *root = cJSON_Parse(data.data());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }
    
    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
    if (!data_obj) {
        ESP_LOGE(TAG, "No data field in JSON");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *req_action = cJSON_GetObjectItem(data_obj, "required_action");
    if (!req_action) {
        ESP_LOGE(TAG, "No required_action field in data");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *submit_tools = cJSON_GetObjectItem(req_action, "submit_tool_outputs");
    if (!submit_tools) {
        ESP_LOGE(TAG, "No submit_tool_outputs field in required_action");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_calls = cJSON_GetObjectItem(submit_tools, "tool_calls");
    if (!tool_calls || cJSON_GetArraySize(tool_calls) == 0) {
        ESP_LOGE(TAG, "No tool_calls array or empty array");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_call = cJSON_GetArrayItem(tool_calls, 0);
    if (!tool_call) {
        ESP_LOGE(TAG, "Failed to get first tool_call");
        cJSON_Delete(root);
        return;
    }
    
    cJSON *tool_call_id = cJSON_GetObjectItem(tool_call, "id");
    if (!tool_call_id || !cJSON_IsString(tool_call_id)) {
        ESP_LOGE(TAG, "No valid id in tool_call");
        cJSON_Delete(root);
        return;
    }

    cJSON *function = cJSON_GetObjectItem(tool_call, "function");
    if (!function) {
        ESP_LOGE(TAG, "No function in tool_call");
        cJSON_Delete(root);
        return;
    }

    cJSON *name = cJSON_GetObjectItem(function, "name");
    if (!name || !cJSON_IsString(name)) {
        ESP_LOGE(TAG, "No valid name in function");
        cJSON_Delete(root);
        return;
    }


    cJSON *arguments = cJSON_GetObjectItem(function, "arguments");
    if (!arguments || !cJSON_IsString(arguments)) {
        ESP_LOGE(TAG, "No valid arguments in function");
        cJSON_Delete(root);
        return;
    }

    // 解析 arguments 字符串，它本身是一个 JSON 字符串
    cJSON *args_json = cJSON_Parse(arguments->valuestring);
    if (!args_json) {
        ESP_LOGE(TAG, "Failed to parse arguments JSON: %s", arguments->valuestring);
        cJSON_Delete(root);
        return;
    }

    
    // 获取conversation_id
    cJSON *conv_id = cJSON_GetObjectItem(data_obj, "conversation_id");
    if (!conv_id || !cJSON_IsString(conv_id)) {
        ESP_LOGE(TAG, "No valid conversation_id");
        cJSON_Delete(args_json);
        cJSON_Delete(root);
        return;
    }

    // 创建一个json
    cJSON *mcp_data = cJSON_CreateObject();
    cJSON *params_data = cJSON_CreateObject();
    cJSON_AddStringToObject(params_data, "conv_id", conv_id->valuestring);
    cJSON_AddStringToObject(params_data, "tool_call_id", tool_call_id->valuestring);
    cJSON_AddStringToObject(params_data, "event_id", event_id);
    
    if (strcmp(name->valuestring, "volume") == 0) {
        cJSON *volume = cJSON_GetObjectItem(args_json, "volume");
        if (volume && cJSON_IsNumber(volume)) {
            // 创建 json
            cJSON_AddStringToObject(mcp_data, "method", "SetVolume");
            cJSON_AddStringToObject(mcp_data, "name", "Speaker");
            cJSON_AddNumberToObject(params_data, "volume", volume->valueint);
        } else {
            ESP_LOGW(TAG, "brightness field not found or not a number");
        }
    } else if (strcmp(name->valuestring, "music_play") == 0) {
        cJSON *url = cJSON_GetObjectItem(args_json, "url");
        if (url && cJSON_IsString(url)) {
            Application::GetInstance().PlayMusic(url->valuestring);
            send_tool_output_response(event_id, 
                                    cJSON_GetStringValue(conv_id), 
                                    cJSON_GetStringValue(tool_call_id), 
                                    "{\\\"music_play_results\\\": \\\"1\\\"}");
        }
    }
    else if (strcmp(name->valuestring, "sleep_control") == 0) {
        Application::GetInstance().QuitTalking();
    } else if (strcmp(name->valuestring, "brightness") == 0) {
        cJSON *brightness = cJSON_GetObjectItem(args_json, "brightness");
        if (brightness && cJSON_IsNumber(brightness)) {
            cJSON_AddStringToObject(mcp_data, "method", "SetBrightness");
            cJSON_AddStringToObject(mcp_data, "name", "Screen");
            cJSON_AddNumberToObject(params_data, "brightness", brightness->valueint);
        }
    }

    cJSON_AddItemToObject(mcp_data, "parameters", params_data);

    // 判断 mcp_data 是否有 method 字段
    if (cJSON_HasObjectItem(mcp_data, "method")) {
        // 执行 MCP
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.Invoke(mcp_data);
    }
    // 释放内存
    cJSON_Delete(args_json);
    cJSON_Delete(root);
    // cJSON_Delete(params_data);
    cJSON_Delete(mcp_data);
}

void CozeMCPParser::send_tool_output_response(const char *event_id, const char *conv_id, const char *tool_call_id, const char *output) {
    // 构建响应
    char response[512];
    snprintf(response, sizeof(response),
            "{"
            "\"id\":\"%s\","
            "\"event_type\":\"conversation.chat.submit_tool_outputs\","
            "\"data\":{"
            "\"chat_id\":\"%s\","
            "\"tool_outputs\":["
            "{"
            "\"tool_call_id\":\"%s\","
            "\"output\":\"%s\""
            "}"
            "]"
            "}"
            "}",
            event_id,
            conv_id,
            tool_call_id,
            output
            );
    
    // esp_websocket_client_handle_t socket_client = get_socket_client();
    // esp_err_t ret = esp_websocket_client_send_text(socket_client, response, strlen(response), portMAX_DELAY);
    // ESP_LOGI(TAG, "send_tool_output_response: %s", response);
    // if (ret > 0) {
    //     ESP_LOGI(TAG, "Init message sent successfully");
    // } else {
    //     ESP_LOGE(TAG, "Failed to send init message: %d", ret);
    // }
}
