cmake_minimum_required(VERSION 3.21)
project(RemoteDesktopClient VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets WebSockets)

qt_add_executable(RemoteDesktopClient WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/SignalingClient.cpp
    src/SignalingClient.h
)
target_link_libraries(RemoteDesktopClient PRIVATE Qt6::Widgets Qt6::WebSockets)
if(MSVC)
    target_compile_options(RemoteDesktopClient PRIVATE /utf-8)
endif()
if(WIN32)
    target_compile_definitions(RemoteDesktopClient PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(RemoteDesktopClient PRIVATE wtsapi32)
endif()

add_custom_command(TARGET RemoteDesktopClient POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config.json"
        "-DDEST_FILE=$<TARGET_FILE_DIR:RemoteDesktopClient>/config.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyConfig.cmake"
    VERBATIM
)
