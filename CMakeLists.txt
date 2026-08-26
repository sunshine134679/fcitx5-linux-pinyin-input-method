cmake_minimum_required(VERSION 3.24)

project(ModernIME VERSION 0.1.0 LANGUAGES CXX)

option(MODERNIME_BUILD_FCITX5 "Build the Fcitx5 addon targets" OFF)
option(MODERNIME_BUILD_TESTS "Build ModernIME tests" ON)
option(MODERNIME_BUILD_TOOLS "Build development and visual verification tools" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ModernIMEWarnings.cmake)

add_subdirectory(core)

if(MODERNIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MODERNIME_BUILD_FCITX5)
    add_subdirectory(adapter/fcitx5)
endif()

add_subdirectory(ui/fcitx5)

if(MODERNIME_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
