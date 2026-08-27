find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

add_executable(modernime-settings
    settings_app.cpp
    data_controller.cpp
    settings_model.cpp
    runtime_controller.cpp
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
)
modernime_enable_warnings(modernime-settings)
