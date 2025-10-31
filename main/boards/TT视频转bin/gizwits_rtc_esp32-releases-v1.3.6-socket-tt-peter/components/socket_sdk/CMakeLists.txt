set(COMPONENT_SRCS 
    "./ble/ble.c" 
    "./ble/gatt_svr.c" 
    "./wifi/wifi.c"
    "./ble/adv.c"
    "./protocol/parse_protocol.c"
    "./services/gsever.c"
    "./storage/storage.c"
    "./ota/ota.c"
    "./mqtt/mqtt.c"
    "./mqtt/mqtt_protocol.c"
    "./server_protocol/pack_server_protocol.c"
    "./server_protocol/parse_server_protocol.c"
    "./services/https_request.c"
    "./protocol/pack_protocol.c"
    "./coze/socket_acion_handler.c"
    "./coze/coze_socket.c"
    "./sentry/error_monitor.c"
    "./config/config.c"
    "./unit/gagent_md5.c"
    "./unit/user_sha256.c"
    "./unit/aes.c"
    "./unit/tool.c"
    "./unit/aesdata.c"
    "./unit/crc16_modbus.c"
    "./sdk_api/sdk_init.c"
    "./sdk_api/sdk_api.c"
    "./sdk_api/sdk_version.c"
    "./wifi/ap_config.cc"
    "./sdk_api/sdk_callback.c"
)

    set(COMPONENT_INCLUDE_DIRS
    "./ble/" 
    "./wifi/"
    "./protocol/"
    "./services/"
    "./storage/"
    "./ota/"
    "./mqtt/"
    "./server_protocol/"
    "./coze/"
    "./unit/"
    "./sentry/"
    "./config/"
    "./sdk_api/"
    )


# 根据配置条件添加语音识别相关的组件
if(NOT CONFIG_WAKE_UP_NONE)
    # list(APPEND COMPONENT_REQUIRES micro_speech)
endif()
idf_component_register(SRCS ${COMPONENT_SRCS}
                     INCLUDE_DIRS "." ${COMPONENT_INCLUDE_DIRS} ${MAIN_INCS_DIRS}
                     REQUIRES espcoredump esp_peripherals protocol_examples_common esp_http_server audio_recorder
                     audio_board mqtt ota_service input_key_service audio_processor audio_pipeline 
                     audio_hal esp_wifi nvs_flash lwip esp_lcd VolcEngineRTCLite bt json driver esp-wifi-connect
                     audio_stream esp_system ${COMPONENT_REQUIRES} main)

# spiffs_create_partition_image(flash_tone ../spiffs FLASH_IN_PROJECT)

#release_remove_tag add_prebuilt_library(VolcEngineRTCLite "libVolcEngineRTCLite.a" REQUIRES mbedtls json zlib)
#release_remove_tag target_link_libraries(${COMPONENT_LIB} PRIVATE VolcEngineRTCLite)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-unused-variable -Wfatal-errors -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-format -Wno-implicit-function-declaration -Wno-pointer-sign -Wno-double-promotion  -Wno-unused-value -Wno-switch -Wno-return-type -g -O3")  
