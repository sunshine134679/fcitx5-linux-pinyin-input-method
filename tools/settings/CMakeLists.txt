find_package(PkgConfig REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(MODERNIME_TIMEOUT_EXECUTABLE timeout REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(FCITX5_UTILS QUIET Fcitx5Utils)

set(modernime_system_fcitx5_addon_dir "")
if(FCITX5_UTILS_FOUND AND FCITX5_UTILS_LIBDIR)
    set(modernime_system_fcitx5_addon_dir
        "${FCITX5_UTILS_LIBDIR}/fcitx5")
endif()

add_library(modernime_settings_components INTERFACE)
target_sources(modernime_settings_components INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/data_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dictionary_pronunciation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/settings_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/package_operation_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/process_runner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/package_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/update_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/clipboard_history_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/overview_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/page_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/settings_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/settings_shell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/settings_shell_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/candidates_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/clipboard_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/diagnostics_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/dictionary_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/input_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/learning_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/overview_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/updates_page.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/settings_window.cpp
)
target_include_directories(modernime_settings_components INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_settings_components INTERFACE
    modernime_core
    modernime_pinyin
    modernime_ui_layout
    PkgConfig::GTK3
)
target_compile_features(modernime_settings_components INTERFACE cxx_std_20)

add_executable(modernime-settings
    settings_app.cpp
)
target_link_libraries(modernime-settings PRIVATE
    modernime_settings_components
)
target_compile_definitions(modernime-settings PRIVATE
    MODERNIME_VERSION="${PROJECT_VERSION}"
    MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    MODERNIME_SYSTEM_FCITX5_ADDON_DIR="${modernime_system_fcitx5_addon_dir}"
)
modernime_enable_warnings(modernime-settings)

install(TARGETS modernime-settings RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-session-init
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-data-guard
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-migrate-channel
    DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/desktop/modernime-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/install_state.sh
              ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/install_lifecycle.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/modernime)
