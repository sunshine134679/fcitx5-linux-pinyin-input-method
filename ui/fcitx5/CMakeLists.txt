find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

add_library(modernime_ui_layout STATIC
    src/candidate_bar_layout.cpp
    src/candidate_bar_renderer.cpp
    src/cairo_render_surface.cpp
)
target_include_directories(modernime_ui_layout PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_ui_layout PUBLIC modernime_core PkgConfig::PANGOCAIRO)
modernime_enable_warnings(modernime_ui_layout)
