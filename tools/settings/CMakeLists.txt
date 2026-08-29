find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(FCITX5_UTILS QUIET Fcitx5Utils)

set(modernime_system_fcitx5_addon_dir "")
if(FCITX5_UTILS_FOUND AND FCITX5_UTILS_LIBDIR)
    set(modernime_system_fcitx5_addon_dir
        "${FCITX5_UTILS_LIBDIR}/fcitx5")
endif()

add_executable(modernime-settings
    settings_app.cpp
    data_controller.cpp
    settings_model.cpp
    runtime_controller.cpp
    clipboard_history_model.cpp
    overview_model.cpp
    page_registry.cpp
    settings_widgets.cpp
    pages/input_page.cpp
    pages/overview_page.cpp
    settings_window.cpp
)
target_include_directories(modernime-settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(modernime-settings PRIVATE
    modernime_core
    modernime_pinyin
    PkgConfig::GTK3
)
target_compile_features(modernime-settings PRIVATE cxx_std_20)
target_compile_definitions(modernime-settings PRIVATE
    MODERNIME_VERSION="${PROJECT_VERSION}"
    MODERNIME_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
    MODERNIME_SYSTEM_FCITX5_ADDON_DIR="${modernime_system_fcitx5_addon_dir}"
)
modernime_enable_warnings(modernime-settings)

install(TARGETS modernime-settings RUNTIME DESTINATION bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/desktop/modernime-settings.desktop
        DESTINATION share/applications)
