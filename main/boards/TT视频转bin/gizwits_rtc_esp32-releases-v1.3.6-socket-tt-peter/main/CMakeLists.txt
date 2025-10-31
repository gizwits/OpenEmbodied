set(COMPONENT_SRCS 
    "./app_main.c" 
    "./board/board_init.c"
    "./board/key.c"
    "./board/gpio.c"
    "./board/charge.c"
    "./board/button.c"
    "./sdk_callback/sdk_callback.c"
    "./storage/user_storage.c"


    # "./soc_src/socmain/user_main.c"
    "./soc_src/app/gat_adapter.c"
    "./soc_src/Gizwits/gizwits_product.c"
    "./soc_src/Gizwits/gizwits_protocol.c"
    "./soc_src/Utils/common.c"
    "./soc_src/Utils/dataPointTools.c"
    "./soc_src/Utils/ringbuffer.c"

    "./uart/uart_ctrl_lcd.c"

    "./mp3_url_play/play_http_mp3_example.c"
    "./hall_switch/hall_switch.c"
    "./tt_led/tt_ledc.c"
    "./battery/battery.c"

    "./power_save/power_save.c"

    )

set( INCS
    "./soc_src/Utils"
    "./soc_src/Gizwits"
    "./soc_src/app"
    "./soc_src/socmain"
    "./uart"
    "./mp3_url_play"
    "./factory_test"
    "./hall_switch"
    "./tt_led"
    "./battery"
    "./power_save"
)
if (CONFIG_FACTORY_TEST_MODE_ENABLE)
    list(APPEND COMPONENT_SRCS
        "./factory_test/factory_test.c"
        "./factory_test/test_stream.c"
    )
endif()

if (CONFIG_AUDIO_BOARD_TOYCORE_DINGSHE)
    list(APPEND COMPONENT_SRCS
    "./board/led_pwm.c"
    "./board/tp_interrupt_main.c"
    )
endif()

if (CONFIG_SERVO_SUPPORT)
    list(APPEND COMPONENT_SRCS
        "./board/steering_engine.c"
    )
endif()

if( DEFINED ENV{USE_SDK_LIB} )
message(STATUS "[**debug**] USE_SDK_LIB ENV defined! ")

idf_component_register(SRCS ${COMPONENT_SRCS}
                     INCLUDE_DIRS "."
                     REQUIRES xtask esp_http_server audio_recorder audio_board mqtt ota_service
                     input_key_service audio_processor audio_pipeline audio_hal esp_wifi 
                     nvs_flash lwip esp_lcd VolcEngineRTCLite bt json driver esp_peripherals 
                     audio_stream esp_system esp_adc ${COMPONENT_REQUIRES})

set(SDK_DEPS esp_peripherals protocol_examples_common esp_http_server audio_recorder
    audio_board mqtt ota_service input_key_service audio_processor audio_pipeline 
    audio_hal esp_wifi nvs_flash lwip esp_lcd VolcEngineRTCLite bt json driver 
    audio_stream esp_system ${COMPONENT_REQUIRES} main)

add_prebuilt_library(socket_sdk "../lib/libsocket_sdk.a" REQUIRES "${SDK_DEPS}")

# `main` calls a function from the library, so link it to `main`
target_link_libraries(${COMPONENT_LIB} PRIVATE socket_sdk)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-unused-variable -Wfatal-errors -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-format -Wno-implicit-function-declaration -Wno-pointer-sign -Wno-double-promotion  -Wno-unused-value -Wno-switch -Wno-return-type -g -O3 -DUSE_SDK_LIB")  


else()

idf_component_register(SRCS ${COMPONENT_SRCS}
                     INCLUDE_DIRS "." ${INCS}
                     REQUIRES  esp_http_server audio_recorder audio_board mqtt ota_service
                     input_key_service audio_processor audio_pipeline audio_hal esp_wifi 
                     nvs_flash lwip esp_lcd VolcEngineRTCLite bt json driver esp_peripherals 
                     audio_stream esp_system esp_adc ${COMPONENT_REQUIRES} socket_sdk)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wno-unused-variable -Wfatal-errors -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-format -Wno-implicit-function-declaration -Wno-pointer-sign -Wno-double-promotion  -Wno-unused-value -Wno-switch -Wno-return-type -g -O3")  

endif()

spiffs_create_partition_image(flash_tone ../spiffs FLASH_IN_PROJECT)

#release_remove_tag add_prebuilt_library(VolcEngineRTCLite "libVolcEngineRTCLite.a" REQUIRES mbedtls json zlib)
#release_remove_tag target_link_libraries(${COMPONENT_LIB} PRIVATE VolcEngineRTCLite)

