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
