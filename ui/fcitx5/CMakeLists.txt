find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

add_library(modernime_ui_layout STATIC
    src/candidate_bar_layout.cpp
    src/candidate_bar_renderer.cpp
    src/cairo_render_surface.cpp
)
set_target_properties(modernime_ui_layout PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(modernime_ui_layout PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_ui_layout PUBLIC modernime_core PkgConfig::PANGOCAIRO)
modernime_enable_warnings(modernime_ui_layout)

if(MODERNIME_BUILD_FCITX5)
    find_package(Fcitx5Core REQUIRED)
    find_package(Fcitx5Utils REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

    add_library(modernime_ui MODULE
        src/ui_addon.cpp
    )
    target_include_directories(modernime_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(modernime_ui PRIVATE
        modernime_ui_layout
        Fcitx5::Core
        Fcitx5::Utils
        PkgConfig::GTK3
    )
    target_compile_features(modernime_ui PRIVATE cxx_std_20)
    modernime_enable_warnings(modernime_ui)
    set_target_properties(modernime_ui PROPERTIES PREFIX "")
endif()
