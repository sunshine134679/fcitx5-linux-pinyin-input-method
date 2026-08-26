add_library(modernime_ui_layout STATIC
    src/candidate_bar_layout.cpp
    src/candidate_bar_renderer.cpp
)
target_include_directories(modernime_ui_layout PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_ui_layout PUBLIC modernime_core)
modernime_enable_warnings(modernime_ui_layout)
