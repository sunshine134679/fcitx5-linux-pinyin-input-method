cmake_minimum_required(VERSION 3.24)

project(ModernIME VERSION 0.1.21 LANGUAGES CXX)

option(MODERNIME_BUILD_FCITX5 "Build the Fcitx5 addon targets" OFF)
option(MODERNIME_BUILD_LIBIME_PINYIN "Build the LibIME-backed pinyin provider" OFF)
option(MODERNIME_BUILD_TESTS "Build ModernIME tests" ON)
option(MODERNIME_BUILD_TOOLS "Build development and visual verification tools" OFF)
option(MODERNIME_BUILD_SETTINGS "Build the GTK3 settings client" OFF)
option(MODERNIME_BUILD_UI_LAYOUT_TESTS "Build UI layout tests" ON)
option(MODERNIME_REQUIRE_FULL_TESTS "Enforce full test requirements and dependencies" OFF)
if(MODERNIME_REQUIRE_FULL_TESTS)
    if(NOT MODERNIME_BUILD_TESTS)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_TESTS=ON")
    endif()
    if(NOT MODERNIME_BUILD_FCITX5)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_FCITX5=ON")
    endif()
    if(NOT MODERNIME_BUILD_LIBIME_PINYIN)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_LIBIME_PINYIN=ON")
    endif()
    if(NOT MODERNIME_BUILD_SETTINGS)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_SETTINGS=ON")
    endif()
    if(NOT MODERNIME_BUILD_UI_LAYOUT_TESTS)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires MODERNIME_BUILD_UI_LAYOUT_TESTS=ON")
    endif()
endif()
if(MODERNIME_BUILD_SETTINGS)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    find_program(MODERNIME_TIMEOUT_EXECUTABLE timeout REQUIRED)
endif()
set(MODERNIME_PACKAGE_CHANNEL "" CACHE STRING "Native package channel identifier (e.g. deb, rpm)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

include(cmake/ModernIMEWarnings.cmake)
include(GNUInstallDirs)

add_subdirectory(core)

if(MODERNIME_BUILD_LIBIME_PINYIN OR MODERNIME_BUILD_SETTINGS)
    if(MODERNIME_BUILD_LIBIME_PINYIN)
        add_subdirectory(data/pinyin)
    endif()
    add_subdirectory(algorithm/pinyin)
endif()

if(MODERNIME_BUILD_FCITX5 OR MODERNIME_BUILD_SETTINGS OR MODERNIME_BUILD_TOOLS OR (MODERNIME_BUILD_TESTS AND MODERNIME_BUILD_UI_LAYOUT_TESTS))
    add_subdirectory(ui/fcitx5)
endif()

if(MODERNIME_BUILD_FCITX5)
    add_subdirectory(adapter/fcitx5)
endif()

if(MODERNIME_BUILD_SETTINGS)
    add_subdirectory(tools/settings)
endif()

if(MODERNIME_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(MODERNIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Pre-exported assets keep clean-clone builds offline and need no image tools.
foreach(MODERNIME_ICON_SIZE IN ITEMS 16 32 48 64 128 256 512)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/icons/${MODERNIME_ICON_SIZE}x${MODERNIME_ICON_SIZE}/modernime.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${MODERNIME_ICON_SIZE}x${MODERNIME_ICON_SIZE}/apps")
endforeach()

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/copyright"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/modernime"
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/extensions/kimpanel@kde.org"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/extensions"
)

if(MODERNIME_PACKAGE_CHANNEL)
    set(MODERNIME_GENERATED_CHANNEL_FILE "${CMAKE_CURRENT_BINARY_DIR}/install-channel.txt")
    file(WRITE "${MODERNIME_GENERATED_CHANNEL_FILE}"
        "channel=${MODERNIME_PACKAGE_CHANNEL}\nversion=${PROJECT_VERSION}\nprefix=${CMAKE_INSTALL_PREFIX}\n"
    )
    install(FILES "${MODERNIME_GENERATED_CHANNEL_FILE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime"
    )
endif()

include(cmake/ModernIMECPack.cmake)

