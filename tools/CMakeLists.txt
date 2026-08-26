add_executable(modernime_candidate_bar_snapshot candidate_bar_snapshot.cpp)
target_include_directories(modernime_candidate_bar_snapshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
target_link_libraries(modernime_candidate_bar_snapshot PRIVATE modernime_ui_layout)
