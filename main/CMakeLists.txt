set(SOURCES "audio/audio_codec.cc"
            "audio/audio_service.cc"
            "audio/codecs/no_audio_codec.cc"
            "audio/codecs/box_audio_codec.cc"
            "audio/codecs/es8311_audio_codec.cc"
            "audio/codecs/es8374_audio_codec.cc"
            "audio/codecs/es8388_audio_codec.cc"
            "audio/codecs/dummy_audio_codec.cc"
            "audio/processors/audio_debugger.cc"
            "led/single_led.cc"
            "led/circular_strip.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/lcd_display.cc"
            "display/oled_display.cc"
            "display/eye_toy_single_display.cc"
            "display/eye_toy_display.cc"
            "display/eye_display.cc"
            "display/eye_display_horizontal.cc"
            "display/eye_display_horizontal_emojis.cc"
            "display/lottie_display.cc"
            "display/lv_mem_psram.c"
            "protocols/protocol.cc"
            "watchdog.cc"
            "protocols/websocket_protocol.cc"
            "iot/thing.cc"
            "iot/thing_manager.cc"
            "server/giz_mqtt.cc"
            "player/player.cc"
            "server/giz_api.cc"
            "server/protocol/iot_protocol.cc"
            "boards/common/data_point_manager.cc"
            "server/protocol/ota_protocol.cc"
            "protocols/mcp.cc"
            "auth.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "ntp.cc"
            "device_state_event.cc"
            "main.cc"
            "factory_test/factory_test.cc"
            "factory_test/test.cc"
            "factory_test/test_uart.cc"
            )

set(INCLUDE_DIRS "." "display" "audio" "protocols")

# 添加 IOT 相关文件
file(GLOB IOT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/iot/things/*.cc)
list(APPEND SOURCES ${IOT_SOURCES})

# 添加板级公共文件
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

# 根据 BOARD_TYPE 配置添加对应的板级文件
if(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)
    set(BOARD_TYPE "bread-compact-wifi")
elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ML307)
    set(BOARD_TYPE "bread-compact-ml307")
elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)
    set(BOARD_TYPE "bread-compact-esp32")
elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32_LCD)
    set(BOARD_TYPE "bread-compact-esp32-lcd")
elseif(CONFIG_BOARD_TYPE_DOIT_AI_01_KIT)
    set(BOARD_TYPE "doit-ai-01-kit")
elseif(CONFIG_BOARD_TYPE_DOIT_AI_01_KIT_NFC)
    set(BOARD_TYPE "doit-ai-01-kit-nfc")
elseif(CONFIG_BOARD_TYPE_DOIT_ESP32S3_EYE)
    set(BOARD_TYPE "doit-esp32s3-eye")
elseif(CONFIG_BOARD_TYPE_DOIT_ESP32S3_EYE_6824)
    set(BOARD_TYPE "doit-esp32s3-eye-6824")
elseif(CONFIG_BOARD_TYPE_GIZWITS_S3_VB6824_LCD)
    set(BOARD_TYPE "gizwits-s3-vb6824-lcd")
elseif(CONFIG_BOARD_TYPE_GIZWITS_S3_VB6824_ST7735S)
    set(BOARD_TYPE "gizwits-s3-vb6824-st7735s")
elseif(CONFIG_BOARD_TYPE_GIZWITS_GOKIT5)
    set(BOARD_TYPE "gizwits-gokit5")
elseif(CONFIG_BOARD_TYPE_GIZWITS_LOTTIE)
    set(BOARD_TYPE "gizwits-lottie")
elseif(CONFIG_BOARD_TYPE_GIZWITS_4KEY)
    set(BOARD_TYPE "gizwits-4key")
elseif(CONFIG_BOARD_TYPE_GIZWITS_4KEY_ML307)
    set(BOARD_TYPE "gizwits-4key-ml307")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824)
    set(BOARD_TYPE "gizwits-c2-6824")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_XINGBAO)
    set(BOARD_TYPE "gizwits-c2-6824-xingbao")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_CWH)
    set(BOARD_TYPE "gizwits-c2-6824-cwh")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_LIGHT_ROBOT)
    set(BOARD_TYPE "gizwits-c2-6824-light-robot")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_ML307)
    set(BOARD_TYPE "gizwits-c2-6824-ml307")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_MOTOR)
    set(BOARD_TYPE "gizwits-c2-6824-motor")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_LVLIN)
    set(BOARD_TYPE "gizwits-c2-6824-lvlin")
elseif(CONFIG_BOARD_TYPE_GIZWITS_C2_6824_TOY)
    set(BOARD_TYPE "gizwits-c2-6824-toy")
elseif(CONFIG_BOARD_TYPE_ESP32S3_XUNGUAN_AMOLED_1_28)
    set(BOARD_TYPE "esp32-s3-xunguan-1.28")
elseif(CONFIG_BOARD_TYPE_ESP32S3_XUNGUAN_AMOLED_1_28_OLD)
    set(BOARD_TYPE "esp32-s3-xunguan-1.28-old")
endif()

# Include board specific directory
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE})

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

if(CONFIG_USE_AUDIO_PROCESSOR)
    list(APPEND SOURCES "audio/processors/afe_audio_processor.cc")
else()
    list(APPEND SOURCES "audio/processors/no_audio_processor.cc")
endif()
if(CONFIG_USE_AFE_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/afe_wake_word.cc")
elseif(CONFIG_USE_ESP_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/esp_wake_word.cc")
elseif(CONFIG_USE_CUSTOM_WAKE_WORD)
    list(APPEND SOURCES "audio/wake_words/custom_wake_word.cc")
endif()

# 根据Kconfig选择语言目录
if(CONFIG_LANGUAGE_ZH_CN)
    set(LANG_DIR "zh-CN")
elseif(CONFIG_LANGUAGE_ZH_CN_LVLIN)
    set(LANG_DIR "zh-CN-LVLIN")
elseif(CONFIG_LANGUAGE_ZH_TW)
    set(LANG_DIR "zh-TW")
elseif(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_JA_JP)
    set(LANG_DIR "ja-JP")
endif()

# 定义生成路径
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/${LANG_DIR}/*.p3)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.p3)

# 如果目标芯片是 ESP32，则排除特定文件
if(CONFIG_IDF_TARGET_ESP32)
    list(REMOVE_ITEM SOURCES "audio/codecs/box_audio_codec.cc"
                             "audio/codecs/es8388_audio_codec.cc"
                             "audio/codecs/es8389_audio_codec.cc"
                             "led/gpio_led.cc"
                             )
endif()

if(CONFIG_IDF_TARGET_ESP32C2)
    list(REMOVE_ITEM SOURCES "display/lv_mem_psram.c")
endif()
           
if(CONFIG_USE_EYE_STYLE_VB6824 OR CONFIG_USE_AUDIO_CODEC_DECODE_OPUS)
    list(APPEND SOURCES "audio/codecs/vb6824_audio_codec.cc")
    list(REMOVE_ITEM SOURCES "audio/codecs/box_audio_codec.cc"
                            "audio/codecs/no_audio_codec.cc"
                            "audio/codecs/es8311_audio_codec.cc"
                            "audio/codecs/es8374_audio_codec.cc"
                            "audio/codecs/es8389_audio_codec.cc"
                            "display/lottie_display.cc"
                            "audio/codecs/es8388_audio_codec.cc")
endif()

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    )

# 使用 target_compile_definitions 来定义 BOARD_TYPE, BOARD_NAME
# 如果 BOARD_NAME 为空，则使用 BOARD_TYPE
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    )

# 添加生成规则
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --input "${LANG_JSON}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# 强制建立生成依赖
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Add Lottie SPIFFS image if board uses lottie
if(CONFIG_LV_USE_LOTTIE)
    # 自动生成 Lottie SPIFFS 镜像
    # 从 main/assets/lottie 目录创建 SPIFFS 镜像
    spiffs_create_partition_image(lottie ${CMAKE_CURRENT_SOURCE_DIR}/assets/lottie FLASH_IN_PROJECT)
endif()

