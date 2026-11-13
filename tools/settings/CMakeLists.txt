find_package(PkgConfig REQUIRED)
find_package(Fcitx5Utils REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(MODERNIME_TIMEOUT_EXECUTABLE timeout REQUIRED)
find_program(MODERNIME_LDD_EXECUTABLE ldd REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c [=[
import pathlib
import re
import ssl

context = ssl.create_default_context()
paths = ssl.get_default_verify_paths()
# OpenSSL loads hashed capath certificates lazily, so an empty list before a
# connection does not mean the system trust store is missing. Inspect those
# certificates in this build-only context; runtime TLS keeps its defaults.
if not context.get_ca_certs() and paths.capath:
    for certificate in pathlib.Path(paths.capath).iterdir():
        if not re.fullmatch(r'[0-9a-fA-F]{8}\.[0-9]+', certificate.name):
            continue
        try:
            context.load_verify_locations(cafile=str(certificate))
        except OSError:
            continue
        if context.get_ca_certs():
            break
assert context.get_ca_certs(), f'请安装 ca-certificates 并检查证书路径：{paths}'
]=]
    RESULT_VARIABLE modernime_https_ready ERROR_VARIABLE modernime_https_error)
if(NOT modernime_https_ready EQUAL 0)
    message(FATAL_ERROR "客户端在线更新需要 Python SSL 和 ca-certificates: ${modernime_https_error}")
endif()
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24)
pkg_check_modules(FCITX5_UTILS QUIET Fcitx5Utils)

set(modernime_system_fcitx5_addon_dir "")
if(FCITX5_UTILS_FOUND AND FCITX5_UTILS_LIBDIR)
    set(modernime_system_fcitx5_addon_dir
        "${FCITX5_UTILS_LIBDIR}/fcitx5")
endif()

add_library(modernime_settings_logic STATIC
    data_controller.cpp
    quick_phrase_store.cpp
    candidate_pin_query.cpp
    dictionary_pronunciation.cpp
    settings_model.cpp
    runtime_controller.cpp
    package_operation_state.cpp
    process_runner.cpp
    package_policy.cpp
    update_controller.cpp
    update_download.cpp
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
    Fcitx5::Utils
    modernime_pinyin
    modernime_ui_layout
    PkgConfig::GTK3
)
target_compile_definitions(modernime_settings_logic PUBLIC
    MODERNIME_VERSION="${PROJECT_VERSION}"
    MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
)
# 5.0.16 修正了 QuickPhrase 带引号文本解析；旧环境保留原有客户端功能。
if(Fcitx5Utils_VERSION VERSION_LESS "5.0.16")
    target_compile_definitions(modernime_settings_logic PRIVATE MODERNIME_LEGACY_QUICK_PHRASE)
endif()
target_compile_features(modernime_settings_logic PUBLIC cxx_std_20)
modernime_enable_warnings(modernime_settings_logic)

add_library(modernime_settings_gtk STATIC
    quick_phrase_editor.cpp
    input_rule_dialog.cpp
    input_assist.cpp
    script_conversion_control.cpp
    symbol_browser.cpp
    candidate_pin_dialog.cpp
    candidate_font_chooser.cpp
    settings_choice.cpp
    settings_widgets.cpp
    settings_motion.cpp
    settings_disclosure.cpp
    release_notes.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-session-runner
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-data-guard
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-migrate-channel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/modernime-update
    DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/desktop/modernime-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/gnome_extension.py
              ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/install_state.sh
              ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/install_lifecycle.sh
        DESTINATION ${CMAKE_INSTALL_DATADIR}/modernime)
