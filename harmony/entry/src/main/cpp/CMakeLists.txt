cmake_minimum_required(VERSION 3.5.0)
project(SubScreen)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH})

set(CMAKE_CXX_STANDARD 17)

add_library(subscreen SHARED
    subscreen_napi.cpp
    net_server.cpp
    video_decoder.cpp
)

target_link_libraries(subscreen PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_media_vdec.so
    libnative_media_codecbase.so
    libnative_media_core.so
    libhilog_ndk.z.so
)
