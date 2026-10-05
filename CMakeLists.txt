cmake_minimum_required(VERSION 3.20)
project(uberdemotools)
set(CMAKE_CXX_STANDARD 14)

link_libraries(pthread rt)
include_directories(UDT_DLL/include)
include_directories(UDT_DLL/src)
include_directories(UDT_DLL/src/apps)
file (GLOB SRC_FILES
UDT_DLL/src/*.h
        UDT_DLL/src/*.hpp
        UDT_DLL/src/*.cpp
        )

add_library(UDT SHARED
        UDT_DLL/src/api.cpp
        ${SRC_FILES}
)

add_executable(udt_cutter
        UDT_DLL/src/apps/app_demo_cutter.cpp
        UDT_DLL/src/apps/shared.cpp
        ${SRC_FILES}
        )

add_executable(udt_json
        UDT_DLL/src/apps/app_demo_json.cpp
        UDT_DLL/src/apps/shared.cpp
${SRC_FILES}
)

add_executable(udt_converter
        UDT_DLL/src/apps/app_demo_converter.cpp
        UDT_DLL/src/apps/shared.cpp
        ${SRC_FILES}
)

add_executable(tut_multi_rail
        UDT_DLL/src/apps/tut_multi_rail.cpp
        ${SRC_FILES}
)

add_executable(tut_players
        UDT_DLL/src/apps/tut_players.cpp
        ${SRC_FILES}
)
