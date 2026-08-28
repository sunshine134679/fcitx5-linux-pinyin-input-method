add_executable(modernime_core_tests core/foundation_test.cpp)
target_link_libraries(modernime_core_tests PRIVATE modernime_core)
add_test(NAME modernime_core_foundation COMMAND modernime_core_tests)

add_executable(modernime_settings_tests core/settings_test.cpp)
target_link_libraries(modernime_settings_tests PRIVATE modernime_core)
add_test(NAME modernime_settings COMMAND modernime_settings_tests)

add_executable(modernime_clipboard_history_tests core/clipboard_history_test.cpp)
target_link_libraries(modernime_clipboard_history_tests PRIVATE modernime_core)
add_test(NAME modernime_clipboard_history COMMAND modernime_clipboard_history_tests)

if(MODERNIME_BUILD_SETTINGS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

    add_executable(modernime_settings_model_tests
        settings/settings_window_model_test.cpp
    )
    target_link_libraries(modernime_settings_model_tests PRIVATE modernime_core)
    target_include_directories(modernime_settings_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_sources(modernime_settings_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/settings_model.cpp
    )
    add_test(NAME modernime_settings_model
        COMMAND modernime_settings_model_tests
    )

    add_executable(modernime_fake_fcitx5_remote
        settings/fake_fcitx5_remote.cpp
    )
    add_executable(modernime_fake_fcitx5
        settings/fake_fcitx5.cpp
    )
    add_executable(modernime_runtime_controller_tests
        settings/runtime_controller_test.cpp
    )
    target_include_directories(modernime_runtime_controller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_sources(modernime_runtime_controller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/runtime_controller.cpp
    )
    target_link_libraries(modernime_runtime_controller_tests PRIVATE
        PkgConfig::GTK3
    )
    add_test(NAME modernime_runtime_controller
        COMMAND modernime_runtime_controller_tests
            $<TARGET_FILE:modernime_fake_fcitx5_remote>
            $<TARGET_FILE:modernime_fake_fcitx5>
    )

    add_executable(modernime_data_controller_tests
        settings/data_controller_test.cpp
    )
    target_include_directories(modernime_data_controller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_sources(modernime_data_controller_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/data_controller.cpp
    )
    target_link_libraries(modernime_data_controller_tests PRIVATE
        modernime_core
        modernime_pinyin
    )
    add_test(NAME modernime_data_controller
        COMMAND modernime_data_controller_tests
    )
endif()

add_executable(modernime_dictionary_prior_tests core/dictionary_prior_test.cpp)
target_link_libraries(modernime_dictionary_prior_tests PRIVATE modernime_core)
add_test(NAME modernime_dictionary_prior COMMAND modernime_dictionary_prior_tests)

add_executable(modernime_learning_store_tests core/learning_store_test.cpp)
target_link_libraries(modernime_learning_store_tests PRIVATE modernime_core)
add_test(NAME modernime_learning_store COMMAND modernime_learning_store_tests)

add_executable(modernime_input_state_tests core/input_state_test.cpp)
target_link_libraries(modernime_input_state_tests PRIVATE modernime_core)
add_test(NAME modernime_input_state COMMAND modernime_input_state_tests)

add_executable(modernime_pinyin_match_tests core/pinyin_match_test.cpp)
target_link_libraries(modernime_pinyin_match_tests PRIVATE modernime_core)
add_test(NAME modernime_pinyin_match COMMAND modernime_pinyin_match_tests)

if(MODERNIME_BUILD_LIBIME_PINYIN)
    add_executable(modernime_user_dictionary_tests
        core/user_dictionary_test.cpp
    )
    target_link_libraries(modernime_user_dictionary_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_user_dictionary
        COMMAND modernime_user_dictionary_tests
    )

    add_executable(modernime_pinyin_provider_tests core/pinyin_provider_test.cpp)
    target_link_libraries(modernime_pinyin_provider_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_pinyin_provider COMMAND modernime_pinyin_provider_tests)

    add_executable(modernime_pinyin_engine_tests
        adapter/pinyin_engine_integration_test.cpp
    )
    target_include_directories(modernime_pinyin_engine_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
    )
    target_sources(modernime_pinyin_engine_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/src/engine.cpp
    )
    target_link_libraries(modernime_pinyin_engine_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_pinyin_engine COMMAND modernime_pinyin_engine_tests)
endif()

add_executable(modernime_adapter_tests adapter/engine_state_test.cpp)
target_include_directories(modernime_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
)
target_sources(modernime_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/src/engine.cpp
)
target_link_libraries(modernime_adapter_tests PRIVATE modernime_core)
add_test(NAME modernime_engine_state COMMAND modernime_adapter_tests)

add_executable(modernime_clipboard_trigger_tests
    adapter/clipboard_trigger_test.cpp
    ../adapter/fcitx5/src/engine.cpp
)
target_include_directories(modernime_clipboard_trigger_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
)
target_link_libraries(modernime_clipboard_trigger_tests PRIVATE modernime_core)
add_test(NAME modernime_clipboard_trigger
    COMMAND modernime_clipboard_trigger_tests
)

if(MODERNIME_BUILD_FCITX5)
    find_package(Fcitx5Core REQUIRED)

    add_executable(modernime_fcitx5_candidate_list_tests
        adapter/fcitx5_candidate_list_test.cpp
    )
    target_link_libraries(modernime_fcitx5_candidate_list_tests
        PRIVATE Fcitx5::Core
    )
    add_test(NAME modernime_fcitx5_candidate_list
        COMMAND modernime_fcitx5_candidate_list_tests
    )

    add_executable(modernime_addon_load_tests adapter/addon_load_test.cpp)
    target_link_libraries(modernime_addon_load_tests PRIVATE ${CMAKE_DL_LIBS})
    add_test(NAME modernime_fcitx5_addon_load
        COMMAND modernime_addon_load_tests $<TARGET_FILE:modernime_fcitx5>
    )

    add_executable(modernime_fcitx5_input_panel_tests
        adapter/fcitx5_input_panel_test.cpp
        ../adapter/fcitx5/src/inputmethod.cpp
        ../adapter/fcitx5/src/engine.cpp
    )
    target_include_directories(modernime_fcitx5_input_panel_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
    )
    target_link_libraries(modernime_fcitx5_input_panel_tests
        PRIVATE modernime_core Fcitx5::Core Fcitx5::Utils
    )
    add_test(NAME modernime_fcitx5_input_panel
        COMMAND modernime_fcitx5_input_panel_tests
    )

    add_executable(modernime_key_translation_tests
        adapter/key_translation_test.cpp
        ../adapter/fcitx5/src/inputmethod.cpp
        ../adapter/fcitx5/src/engine.cpp
    )
    target_include_directories(modernime_key_translation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
    )
    target_link_libraries(modernime_key_translation_tests
        PRIVATE modernime_core Fcitx5::Core Fcitx5::Utils
    )
    add_test(NAME modernime_key_translation
        COMMAND modernime_key_translation_tests
    )
endif()

add_executable(modernime_ui_tests ui/candidate_bar_layout_test.cpp)
target_include_directories(modernime_ui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
target_link_libraries(modernime_ui_tests PRIVATE modernime_core)
target_sources(modernime_ui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_layout.cpp
)
add_test(NAME modernime_candidate_bar_layout COMMAND modernime_ui_tests)

add_executable(modernime_window_anchor_tests ui/window_anchor_test.cpp)
target_include_directories(modernime_window_anchor_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
add_test(NAME modernime_window_anchor COMMAND modernime_window_anchor_tests)

add_executable(modernime_status_indicator_tests ui/status_indicator_test.cpp)
target_include_directories(modernime_status_indicator_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
add_test(NAME modernime_status_indicator COMMAND modernime_status_indicator_tests)

add_executable(modernime_candidate_bar_renderer_tests ui/candidate_bar_renderer_test.cpp)
target_include_directories(modernime_candidate_bar_renderer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
target_sources(modernime_candidate_bar_renderer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_layout.cpp
)
target_link_libraries(modernime_candidate_bar_renderer_tests PRIVATE modernime_core)
add_test(NAME modernime_candidate_bar_renderer COMMAND modernime_candidate_bar_renderer_tests)

add_executable(modernime_cairo_surface_tests ui/cairo_surface_test.cpp)
target_include_directories(modernime_cairo_surface_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
)
target_link_libraries(modernime_cairo_surface_tests PRIVATE modernime_ui_layout)
add_test(NAME modernime_cairo_surface COMMAND modernime_cairo_surface_tests)

find_program(BASH_EXECUTABLE bash)
if(BASH_EXECUTABLE)
    add_test(NAME modernime_repository_contract
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/repository_contract_test.sh
    )
    add_test(NAME modernime_install_runtime
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_runtime_test.sh
    )
    add_test(NAME modernime_install_environment
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_environment_test.sh
    )
    add_test(NAME modernime_settings_install
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/settings/settings_install_test.sh
    )
endif()
