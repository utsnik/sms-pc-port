cmake_minimum_required(VERSION 3.20)
project(sms_gx2_texture_pack LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SMS_GX_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../platform/gx/src")
add_library(gx2_texture_pack_core STATIC
    "${SMS_GX_SRC}/gx_texture_pack_common.cpp"
    "${SMS_GX_SRC}/gx2_texture_pack_format.cpp"
    "${SMS_GX_SRC}/gx_bcenc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gx2_texture_pack.cpp")
target_compile_definitions(gx2_texture_pack_core PRIVATE GX2_TEXTURE_PACK_NO_MAIN)
target_include_directories(gx2_texture_pack_core PRIVATE "${SMS_GX_SRC}")

add_executable(gx2_texture_pack gx2_texture_pack.cpp)
target_link_libraries(gx2_texture_pack PRIVATE gx2_texture_pack_core)
target_include_directories(gx2_texture_pack PRIVATE "${SMS_GX_SRC}")

enable_testing()
add_executable(gx2_texture_pack_format_test
    "${CMAKE_CURRENT_SOURCE_DIR}/../platform/gx/tests/gx2_texture_pack_format_test.cpp"
    "${SMS_GX_SRC}/gx2_texture_pack_format.cpp")
target_include_directories(gx2_texture_pack_format_test PRIVATE "${SMS_GX_SRC}")
add_test(NAME gx2_texture_pack_format_test COMMAND gx2_texture_pack_format_test)

add_executable(gx2_texture_pack_test
    "${CMAKE_CURRENT_SOURCE_DIR}/../platform/gx/tests/gx2_texture_pack_test.cpp")
target_link_libraries(gx2_texture_pack_test PRIVATE gx2_texture_pack_core)
target_include_directories(gx2_texture_pack_test PRIVATE "${SMS_GX_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME gx2_texture_pack_test COMMAND gx2_texture_pack_test)

add_executable(gx2_texture_pack_runtime_test
    "${CMAKE_CURRENT_SOURCE_DIR}/../platform/gx/tests/gx2_texture_pack_runtime_test.cpp")
target_include_directories(gx2_texture_pack_runtime_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../platform/wiiu")
add_test(NAME gx2_texture_pack_runtime_test COMMAND gx2_texture_pack_runtime_test)
