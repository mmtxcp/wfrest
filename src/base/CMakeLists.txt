cmake_minimum_required(VERSION 3.6)
project(base)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
find_package(ZLIB REQUIRED)

set(SRC
    base64.cc
    ErrorCode.cc
    Compress.cc
    SysInfo.cc     
    Timestamp.cc
)

add_library(${PROJECT_NAME} OBJECT ${SRC})
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
