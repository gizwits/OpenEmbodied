#
# Main component of TF Micro project 'micro_speech'.
#

idf_component_register(
    SRCS src/main_functions.cc src/audio_provider.cc src/feature_provider.cc
         src/model.cc src/recognize_commands.cc src/command_responder.cc
         src/micro_features_generator.cc
         src/audio_preprocessor_int8_model_data.cc
    PRIV_REQUIRES spi_flash driver esp_timer audio_pipeline audio_stream
    INCLUDE_DIRS "")

add_prebuilt_library(REQUIRES espressif__esp-dsp espressif__esp-nn espressif__esp-tflite-micro)

    # Reduce the level of paranoia to be able to compile sources
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-maybe-uninitialized
    -Wno-missing-field-initializers
    -Wno-error=sign-compare
    -Wno-error=double-promotion
    -Wno-type-limits)
