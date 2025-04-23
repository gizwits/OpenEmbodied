#pragma once

#include <stdbool.h>
#include <string_view>
#include "cJSON.h"

class CozeMCPParser {
public:
    static CozeMCPParser& getInstance();
    
    void handle_mcp(std::string_view data);
    
private:
    CozeMCPParser() = default;
    ~CozeMCPParser() = default;
    
    // Delete copy constructor and assignment operator
    CozeMCPParser(const CozeMCPParser&) = delete;
    CozeMCPParser& operator=(const CozeMCPParser&) = delete;
    
    void send_tool_output_response(const char *event_id, const char *conv_id, const char *tool_call_id, const char *output);
    
    static const char* TAG;
};
