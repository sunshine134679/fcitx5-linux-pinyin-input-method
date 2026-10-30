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

add_library(modernime_settings_logic STATIC
    data_controller.cpp
    dictionary_pronunciation.cpp
    settings_model.cpp
    runtime_controller.cpp
    package_operation_state.cpp
    process_runner.cpp
    package_policy.cpp
    update_controller.cpp
    clipboard_history_model.cpp
    overview_model.cpp
    page_registry.cpp
    settings_shell_state.cpp
)
target_include_directories(modernime_settings_logic PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_settings_logic PUBLIC
    modernime_core
    modernime_pinyin
    modernime_ui_layout
    PkgConfig::GTK3
)
target_compile_definitions(modernime_settings_logic PUBLIC
    MODERNIME_VERSION="${PROJECT_VERSION}"
    MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
)
target_compile_features(modernime_settings_logic PUBLIC cxx_std_20)
modernime_enable_warnings(modernime_settings_logic)

add_library(modernime_settings_gtk STATIC
    settings_widgets.cpp
    settings_shell.cpp
    settings_window.cpp
    pages/candidates_page.cpp
    pages/clipboard_page.cpp
    pages/diagnostics_page.cpp
    pages/dictionary_page.cpp
    pages/input_page.cpp
    pages/learning_page.cpp
    pages/overview_page.cpp
    pages/updates_page.cpp
)
target_include_directories(modernime_settings_gtk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_settings_gtk PUBLIC
    modernime_settings_logic
    PkgConfig::GTK3
)
target_compile_definitions(modernime_settings_gtk PRIVATE
    MODERNIME_SYSTEM_FCITX5_ADDON_DIR="${modernime_system_fcitx5_addon_dir}"
)
target_compile_features(modernime_settings_gtk PUBLIC cxx_std_20)
modernime_enable_warnings(modernime_settings_gtk)

add_library(modernime_settings_components INTERFACE)
target_link_libraries(modernime_settings_components INTERFACE
    modernime_settings_gtk
)

add_executable(modernime-settings
    settings_app.cpp
)
target_link_libraries(modernime-settings PRIVATE
    modernime_settings_gtk
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
