set(SRC_FILES "audio_processor.c")

list(APPEND  SRC_FILES "audio_stream_single_microphone.c" "audio_log.c" "audio_sys.c")


set(COMPONENT_SRCS ${SRC_FILES})
set(COMPONENT_ADD_INCLUDEDIRS include)
set(COMPONENT_REQUIRES xtask audio_recorder audio_stream audio_pipeline audio_hal esp_peripherals main)

register_component()

add_definitions(-DENABLE_AUDIO_STREAM_SINGLE_MIC)
