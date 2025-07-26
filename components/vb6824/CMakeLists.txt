set(incs )
set(srcs )
set(assets )
list(APPEND srcs
        vb6824.c
        )

list(APPEND incs
        .
        )

        
if(CONFIG_VB6824_OTA_SUPPORT)
list(APPEND assets assets/update.html)
list(APPEND srcs https.c)
endif()

idf_component_register(SRCS "${srcs}"
                            INCLUDE_DIRS "${incs}"
                            EMBED_TXTFILES "${assets}"
                            REQUIRES driver esp_wifi esp_http_client esp_http_server json esp_timer)



if(CONFIG_IDF_TARGET_ESP32)
    set(lib_target "esp32")
elseif(CONFIG_IDF_TARGET_ESP32S2)
    set(lib_target "esp32s2")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    set(lib_target "esp32s3")
    elseif(CONFIG_IDF_TARGET_ESP32C2)
        set(lib_target "esp32c2")
elseif(CONFIG_IDF_TARGET_ESP32C3)
    set(lib_target "esp32c3")
endif()


add_prebuilt_library(vbota "libs/lib${lib_target}.a")
target_link_libraries(${COMPONENT_LIB} PRIVATE vbota)