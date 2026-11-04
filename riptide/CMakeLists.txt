# Riptide: the static library (C++ core), and its C API with Granite's video
# scaler. Granite comes from checkout_granite.sh (into ./Granite). The Metal
# port (metal/) is built by the clients' own build scripts.
cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(riptide LANGUAGES CXX C)

set(RIPTIDE_CXX_FLAGS -Wshadow -Wall -Wextra -Wno-comment -Wno-missing-field-initializers -Wno-empty-body -fvisibility=hidden)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(RIPTIDE_CXX_FLAGS ${RIPTIDE_CXX_FLAGS} -msse3)
endif()

set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_FFMPEG OFF CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)
add_subdirectory(Granite EXCLUDE_FROM_ALL)

add_library(riptide STATIC
        riptide_config.hpp shaders/slangmosh.hpp
        riptide_encoder.hpp riptide_encoder.cpp
        riptide_decoder.hpp riptide_decoder.cpp
        riptide_common.hpp riptide_common.cpp
        riptide_inter.hpp riptide_inter.cpp)
target_include_directories(riptide PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(riptide PRIVATE ${RIPTIDE_CXX_FLAGS})
target_compile_definitions(riptide PRIVATE RIPTIDE_PRECISION=1)
target_link_libraries(riptide PRIVATE granite-vulkan granite-math)

add_library(riptide-c STATIC riptide_c.cpp riptide.h
        Granite/video/scaler.cpp Granite/video/scaler.hpp shaders/slangmosh_scaler.hpp)
target_link_libraries(riptide-c PRIVATE riptide granite-vulkan granite-math)
target_compile_definitions(riptide-c PRIVATE RIPTIDE_EXPORT_SYMBOLS)
target_compile_options(riptide-c PRIVATE ${RIPTIDE_CXX_FLAGS})
target_include_directories(riptide-c PRIVATE shaders Granite/video)

# The Vulkan test encoder (tools/rtenc.c): encodes a clip as the host does.
add_executable(rtenc tools/rtenc.c)
target_link_libraries(rtenc PRIVATE riptide-c riptide granite-vulkan granite-math granite-util granite-volk -pthread -ldl)
target_include_directories(rtenc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} Granite/third_party/khronos/vulkan-headers/include)
