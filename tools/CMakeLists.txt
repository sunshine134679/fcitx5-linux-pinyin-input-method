add_executable(modernime_candidate_bar_snapshot candidate_bar_snapshot.cpp)
target_include_directories(modernime_candidate_bar_snapshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
target_link_libraries(modernime_candidate_bar_snapshot PRIVATE modernime_ui_layout)

if(TARGET modernime_pinyin)
    add_executable(modernime_pinyin_diagnose pinyin_diagnose.cpp)
    target_link_libraries(modernime_pinyin_diagnose PRIVATE modernime_pinyin)
    target_compile_definitions(modernime_pinyin_diagnose PRIVATE
        MODERNIME_DIAG_VERSION="${PROJECT_VERSION}"
        MODERNIME_DIAG_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    modernime_enable_warnings(modernime_pinyin_diagnose)
endif()
