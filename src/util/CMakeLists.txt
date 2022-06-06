cmake_minimum_required(VERSION 3.6)
project(util)

include_directories(${CMAKE_CURRENT_BINARY_DIR})


set(SRC
    FileUtil.cc
    MysqlUtil.cc
    PathUtil.cc 
    StrUtil.cc
    UriUtil.cc
)

add_library(${PROJECT_NAME} OBJECT ${SRC})

if (WIN32)
target_compile_definitions(
		${PROJECT_NAME} PRIVATE
		strdup=_strdup
		strcasecmp=_stricmp
		strncasecmp=_strnicmp
	)
endif ()