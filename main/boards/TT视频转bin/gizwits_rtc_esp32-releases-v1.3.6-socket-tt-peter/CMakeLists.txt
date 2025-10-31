# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

message(STATUS "[**debug**] Using CMake version ${CMAKE_VERSION}")

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-format)
add_compile_options(-Wno-dangling-pointer)
add_compile_options(-Wno-implicit-function-declaration)
project(GizwitsRTC)

# add_compile_definitions(LWIP_DEBUG=1)

set(KCONFIG_PROJECT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/main/Kconfig.projbuild")