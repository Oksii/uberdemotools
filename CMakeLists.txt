cmake_minimum_required(VERSION 3.16)
project(uberdemotools CXX)

# Linux build. Windows builds keep using UDT_DLL/premake.

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

set(UDT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/UDT_DLL)
file(GLOB UDT_CORE_SOURCES CONFIGURE_DEPENDS ${UDT_DIR}/src/*.cpp)

add_library(udt_core OBJECT ${UDT_CORE_SOURCES})
set_target_properties(udt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(udt_core PUBLIC ${UDT_DIR}/include ${UDT_DIR}/src ${UDT_DIR}/src/apps)
target_compile_definitions(udt_core PUBLIC UDT_CREATE_DLL $<$<CONFIG:Debug>:DEBUG> $<$<CONFIG:Debug>:_DEBUG>)
target_compile_options(udt_core PUBLIC -fno-rtti -fno-exceptions -Wno-invalid-offsetof -Wno-narrowing)
target_link_libraries(udt_core PUBLIC Threads::Threads rt)

add_library(UDT SHARED)
target_link_libraries(UDT PUBLIC udt_core)

function(udt_app name source)
	add_executable(${name} ${UDT_DIR}/src/apps/${source} ${UDT_DIR}/src/apps/shared.cpp)
	target_link_libraries(${name} PRIVATE udt_core)
endfunction()

udt_app(UDT_cutter app_demo_cutter.cpp)
udt_app(UDT_splitter app_demo_splitter.cpp)
udt_app(UDT_timeshifter app_demo_time_shifter.cpp)
udt_app(UDT_merger app_demo_merger.cpp)
udt_app(UDT_json app_demo_json.cpp)
udt_app(UDT_captures app_demo_captures.cpp)
udt_app(UDT_converter app_demo_converter.cpp)

foreach(tut tut_players tut_multi_rail)
	add_executable(${tut} ${UDT_DIR}/src/apps/${tut}.cpp)
	target_link_libraries(${tut} PRIVATE UDT)
endforeach()

install(TARGETS UDT UDT_cutter UDT_splitter UDT_timeshifter UDT_merger UDT_json UDT_captures UDT_converter
	RUNTIME DESTINATION bin
	LIBRARY DESTINATION lib)
install(FILES ${UDT_DIR}/include/uberdemotools.h DESTINATION include)
