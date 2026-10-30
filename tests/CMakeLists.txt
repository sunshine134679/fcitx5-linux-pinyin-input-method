# Test assertions must run in optimized builds too; NDEBUG must only affect
# production targets, not turn regression checks into no-ops.
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()

enable_language(C)
find_package(SQLite3 REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME modernime_required_tests_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/required_tests_contract_test.py"
)

add_executable(modernime_settings_tests core/settings_test.cpp)
target_link_libraries(modernime_settings_tests PRIVATE modernime_core)
add_test(NAME modernime_settings COMMAND modernime_settings_tests)

add_executable(modernime_db_fixture_helper core/db_fixture_helper.cpp)
target_link_libraries(modernime_db_fixture_helper PRIVATE modernime_core SQLite::SQLite3 LMDB::LMDB)

if(MODERNIME_BUILD_UI_LAYOUT_TESTS)
    add_test(NAME modernime_configuration_contract
        COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/configuration_contract_test.sh" --build-dir "${CMAKE_BINARY_DIR}"
    )
    set_tests_properties(modernime_configuration_contract PROPERTIES
        TIMEOUT 300
        RUN_SERIAL TRUE
    )
endif()

add_executable(modernime_clipboard_history_tests core/clipboard_history_test.cpp)
target_link_libraries(modernime_clipboard_history_tests PRIVATE modernime_core)
add_test(NAME modernime_clipboard_history COMMAND modernime_clipboard_history_tests)

add_executable(modernime_persistent_clipboard_history_tests
    core/persistent_clipboard_history_test.cpp
)
target_link_libraries(modernime_persistent_clipboard_history_tests
    PRIVATE modernime_core
)
add_test(NAME modernime_persistent_clipboard_history
    COMMAND modernime_persistent_clipboard_history_tests
)

if(MODERNIME_BUILD_SETTINGS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

    add_executable(modernime_dictionary_pronunciation_tests
        settings/dictionary_pronunciation_test.cpp
    )
    target_link_libraries(modernime_dictionary_pronunciation_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_dictionary_pronunciation
        COMMAND modernime_dictionary_pronunciation_tests)

    add_executable(modernime_settings_model_tests
        settings/settings_window_model_test.cpp
    )
    target_link_libraries(modernime_settings_model_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_settings_model
        COMMAND modernime_settings_model_tests
    )

    add_executable(modernime_input_page_state_tests
        settings/input_page_state_test.cpp
    )
    target_link_libraries(modernime_input_page_state_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_input_page_state
        COMMAND modernime_input_page_state_tests
    )

    add_executable(modernime_shortcut_recorder_tests
        settings/shortcut_recorder_test.cpp
    )
    target_link_libraries(modernime_shortcut_recorder_tests PRIVATE
        modernime_settings_gtk
    )
    add_test(NAME modernime_shortcut_recorder
        COMMAND modernime_shortcut_recorder_tests
    )

    add_executable(modernime_clipboard_page_contract_tests
        settings/clipboard_page_contract_test.cpp
    )
    target_include_directories(modernime_clipboard_page_contract_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_link_libraries(modernime_clipboard_page_contract_tests PRIVATE
        modernime_core
    )
    add_test(NAME modernime_clipboard_page_contract
        COMMAND modernime_clipboard_page_contract_tests
    )

    add_executable(modernime_learning_page_contract_tests
        settings/learning_page_contract_test.cpp
    )
    target_link_libraries(modernime_learning_page_contract_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_learning_page_contract
        COMMAND modernime_learning_page_contract_tests
    )

    add_executable(modernime_dictionary_page_contract_tests
        settings/dictionary_page_contract_test.cpp
    )
    target_link_libraries(modernime_dictionary_page_contract_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_dictionary_page_contract
        COMMAND modernime_dictionary_page_contract_tests
    )

    add_executable(modernime_diagnostics_page_contract_tests
        settings/diagnostics_page_contract_test.cpp
    )
    target_include_directories(modernime_diagnostics_page_contract_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_link_libraries(modernime_diagnostics_page_contract_tests PRIVATE
        modernime_core
    )
    add_test(NAME modernime_diagnostics_page_contract
        COMMAND modernime_diagnostics_page_contract_tests
    )

    add_executable(modernime_candidates_page_contract_tests
        settings/candidates_page_contract_test.cpp
    )
    target_link_libraries(modernime_candidates_page_contract_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_candidates_page_contract
        COMMAND modernime_candidates_page_contract_tests
    )

    add_executable(modernime_updates_page_contract_tests
        settings/updates_page_contract_test.cpp
    )
    target_link_libraries(modernime_updates_page_contract_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_updates_page_contract
        COMMAND modernime_updates_page_contract_tests
    )

    add_executable(modernime_diagnostics_lifetime_tests
        settings/diagnostics_lifetime_test.cpp
    )
    target_include_directories(modernime_diagnostics_lifetime_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    add_test(NAME modernime_diagnostics_lifetime
        COMMAND modernime_diagnostics_lifetime_tests
    )

    add_executable(modernime_settings_ui_contract_tests
        settings/settings_ui_contract_test.cpp
    )
    target_link_libraries(modernime_settings_ui_contract_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_settings_ui_contract
        COMMAND modernime_settings_ui_contract_tests
    )

    add_executable(modernime_settings_shell_state_tests
        settings/settings_shell_state_test.cpp
    )
    target_link_libraries(modernime_settings_shell_state_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_settings_shell_state
        COMMAND modernime_settings_shell_state_tests
    )

    add_executable(modernime_settings_gtk_raii_tests
        settings/settings_gtk_raii_test.cpp
    )
    target_include_directories(modernime_settings_gtk_raii_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
    )
    target_link_libraries(modernime_settings_gtk_raii_tests PRIVATE
        PkgConfig::GTK3
    )
    add_test(NAME modernime_settings_gtk_raii
        COMMAND modernime_settings_gtk_raii_tests
    )

    add_executable(modernime_settings_focus_gtk_tests
        settings/settings_focus_gtk_test.cpp
    )
    target_link_libraries(modernime_settings_focus_gtk_tests PRIVATE
        modernime_settings_gtk
    )
    add_test(NAME modernime_settings_focus_gtk
        COMMAND modernime_settings_focus_gtk_tests
    )
    set_tests_properties(modernime_settings_focus_gtk PROPERTIES
        ENVIRONMENT "GTK_IM_MODULE=gtk-im-context-simple"
        RESOURCE_LOCK modernime_gtk_display
        SKIP_RETURN_CODE 77
    )

    add_executable(modernime_settings_shell_gtk_tests
        settings/settings_shell_gtk_test.cpp
    )
    target_link_libraries(modernime_settings_shell_gtk_tests PRIVATE
        modernime_settings_components
    )
    target_compile_definitions(modernime_settings_shell_gtk_tests PRIVATE
        MODERNIME_SYSTEM_FCITX5_ADDON_DIR=""
    )
    add_test(NAME modernime_settings_shell_gtk
        COMMAND modernime_settings_shell_gtk_tests
    )
    set_tests_properties(modernime_settings_shell_gtk PROPERTIES
        ENVIRONMENT "GTK_IM_MODULE=gtk-im-context-simple"
        RESOURCE_LOCK modernime_gtk_display
        SKIP_RETURN_CODE 77
        TIMEOUT 20
    )

    add_executable(modernime_settings_save_gtk_tests
        settings/settings_save_gtk_test.cpp
    )
    target_link_libraries(modernime_settings_save_gtk_tests PRIVATE
        modernime_settings_components
    )
    target_compile_definitions(modernime_settings_save_gtk_tests PRIVATE
        MODERNIME_SYSTEM_FCITX5_ADDON_DIR=""
    )
    add_test(NAME modernime_settings_save_gtk
        COMMAND modernime_settings_save_gtk_tests
    )
    set_tests_properties(modernime_settings_save_gtk PROPERTIES
        ENVIRONMENT "GTK_IM_MODULE=gtk-im-context-simple"
        RESOURCE_LOCK modernime_gtk_display
        SKIP_RETURN_CODE 77
        TIMEOUT 20
    )

    add_executable(modernime_settings_data_async_gtk_tests
        settings/settings_data_async_gtk_test.cpp
    )
    target_link_libraries(modernime_settings_data_async_gtk_tests PRIVATE
        modernime_settings_components
    )
    target_compile_definitions(modernime_settings_data_async_gtk_tests PRIVATE
        MODERNIME_SYSTEM_FCITX5_ADDON_DIR=""
    )
    add_test(NAME modernime_settings_data_async_gtk
        COMMAND modernime_settings_data_async_gtk_tests
    )
    set_tests_properties(modernime_settings_data_async_gtk PROPERTIES
        ENVIRONMENT "GTK_IM_MODULE=gtk-im-context-simple"
        RESOURCE_LOCK modernime_gtk_display
        SKIP_RETURN_CODE 77
        TIMEOUT 20
    )

    add_executable(modernime_update_controller_tests
        settings/update_controller_test.cpp
    )
    target_link_libraries(modernime_update_controller_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_update_controller COMMAND modernime_update_controller_tests)

    add_executable(modernime_process_runner_tests
        settings/process_runner_test.cpp
    )
    target_link_libraries(modernime_process_runner_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_process_runner COMMAND modernime_process_runner_tests)

    add_test(NAME modernime_install_gate_matrix
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_install_gate.py"
                --binary "$<TARGET_FILE:modernime-settings>"
    )

    add_executable(modernime_settings_widgets_contract_tests
        settings/settings_widgets_contract_test.cpp
    )
    target_link_libraries(modernime_settings_widgets_contract_tests PRIVATE
        modernime_settings_gtk
    )
    add_test(NAME modernime_settings_widgets_contract
        COMMAND modernime_settings_widgets_contract_tests
    )

    add_executable(modernime_page_registry_tests
        settings/page_registry_test.cpp
    )
    target_link_libraries(modernime_page_registry_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_page_registry COMMAND modernime_page_registry_tests)

    add_test(NAME modernime_settings_navigation_dbus
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/settings/settings_navigation_dbus_test.sh
                $<TARGET_FILE:modernime-settings>
    )
    set_tests_properties(modernime_settings_navigation_dbus PROPERTIES
        RESOURCE_LOCK modernime_gtk_display
        SKIP_RETURN_CODE 77
        TIMEOUT 30
    )

    add_executable(modernime_clipboard_history_model_tests
        settings/clipboard_history_model_test.cpp
    )
    target_link_libraries(modernime_clipboard_history_model_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_clipboard_history_model
        COMMAND modernime_clipboard_history_model_tests
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
    target_link_libraries(modernime_runtime_controller_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_runtime_controller
        COMMAND modernime_runtime_controller_tests
            $<TARGET_FILE:modernime_fake_fcitx5_remote>
            $<TARGET_FILE:modernime_fake_fcitx5>
    )
    add_test(NAME modernime_runtime_controller_concurrency
        COMMAND "${BASH_EXECUTABLE}" ${CMAKE_CURRENT_SOURCE_DIR}/settings/runtime_controller_concurrency_test.sh
            "${CMAKE_BINARY_DIR}"
    )

    add_executable(modernime_data_controller_tests
        settings/data_controller_test.cpp
    )
    target_link_libraries(modernime_data_controller_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_data_controller
        COMMAND modernime_data_controller_tests
    )

    add_executable(modernime_overview_model_tests
        settings/overview_model_test.cpp
    )
    target_link_libraries(modernime_overview_model_tests PRIVATE
        modernime_settings_logic
    )
    add_test(NAME modernime_overview_model
        COMMAND modernime_overview_model_tests
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

add_executable(modernime_english_dictionary_tests core/english_dictionary_test.cpp)
target_link_libraries(modernime_english_dictionary_tests PRIVATE modernime_core)
add_test(NAME modernime_english_dictionary COMMAND modernime_english_dictionary_tests)

add_executable(modernime_english_definition_tests core/english_definition_test.cpp)
target_link_libraries(modernime_english_definition_tests PRIVATE modernime_core)
target_compile_definitions(modernime_english_definition_tests PRIVATE
    MODERNIME_ENGLISH_DICT_FILE="${CMAKE_SOURCE_DIR}/data/pinyin/modernime-english-dict.bin"
)
add_test(NAME modernime_english_definition COMMAND modernime_english_definition_tests)

add_executable(modernime_pinyin_match_tests core/pinyin_match_test.cpp)
target_link_libraries(modernime_pinyin_match_tests PRIVATE modernime_core)
add_test(NAME modernime_pinyin_match COMMAND modernime_pinyin_match_tests)

add_executable(modernime_punctuation_tests core/punctuation_test.cpp)
target_link_libraries(modernime_punctuation_tests PRIVATE modernime_core)
add_test(NAME modernime_punctuation COMMAND modernime_punctuation_tests)

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

    add_executable(modernime_english_arbitration_tests core/english_arbitration_test.cpp)
    target_link_libraries(modernime_english_arbitration_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_english_arbitration COMMAND modernime_english_arbitration_tests)

    add_executable(modernime_pinyin_data_dirs_tests core/pinyin_data_dirs_test.cpp)
    target_link_libraries(modernime_pinyin_data_dirs_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_pinyin_data_dirs COMMAND modernime_pinyin_data_dirs_tests)

    add_executable(modernime_pinyin_quality_tests core/pinyin_quality_test.cpp)
    target_link_libraries(modernime_pinyin_quality_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_pinyin_quality COMMAND modernime_pinyin_quality_tests)

    add_executable(modernime_pinyin_daily_simulation_tests core/pinyin_daily_simulation_test.cpp)
    target_link_libraries(modernime_pinyin_daily_simulation_tests PRIVATE modernime_pinyin)
    add_test(NAME modernime_pinyin_daily_simulation COMMAND modernime_pinyin_daily_simulation_tests)

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

    if(NOT MODERNIME_SIMULATE_MISSING_GTEST)
        find_package(Threads REQUIRED)
        find_path(GTEST_INCLUDE_DIR
            NAMES gtest/gtest.h
            HINTS
                "$ENV{HOME}/.local/include"
                "/home/$ENV{USER}/.local/include"
        )
        find_library(GTEST_LIBRARY
            NAMES libgtest.a gtest
            HINTS
                "$ENV{HOME}/.local/lib"
                "/home/$ENV{USER}/.local/lib"
        )
        find_library(GTEST_MAIN_LIBRARY
            NAMES libgtest_main.a gtest_main
            HINTS
                "$ENV{HOME}/.local/lib"
                "/home/$ENV{USER}/.local/lib"
        )
    endif()
    if(GTEST_INCLUDE_DIR AND GTEST_LIBRARY AND GTEST_MAIN_LIBRARY)
        add_executable(modernime_personal_engine_gtest
            core/personal_engine_gtest.cpp
            core/personalization_snapshot_regression_test.cpp
            core/personalization_storage_regression_test.cpp
            core/personalization_provider_regression_test.cpp
            core/personalization_controller_regression_test.cpp
            core/candidate_ranking_regression_test.cpp
            core/learning_writer_concurrency_test.cpp
            core/lmdb_lifecycle_test.cpp
            core/learning_semantics_test.cpp
            core/personal_engine_file_test.cpp
            core/learning_order_test.cpp
            core/candidate_identity_regression_test.cpp
            core/pinyin_composition_trace_test.cpp
            core/pinyin_resource_coordination_test.cpp
            adapter/candidate_selection_test.cpp
            ../adapter/fcitx5/src/engine.cpp
        )
        target_include_directories(modernime_personal_engine_gtest PRIVATE
            ${GTEST_INCLUDE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
        )
        target_link_libraries(modernime_personal_engine_gtest PRIVATE
            modernime_pinyin
            modernime_core
            ${GTEST_MAIN_LIBRARY}
            ${GTEST_LIBRARY}
            Threads::Threads
        )
        if(MODERNIME_BUILD_FCITX5)
            target_link_libraries(modernime_personal_engine_gtest PRIVATE Fcitx5::Core Fcitx5::Utils modernime_ui_layout)
            target_compile_definitions(modernime_personal_engine_gtest PRIVATE MODERNIME_HAS_FCITX5=1)
        endif()
        add_test(NAME modernime_personal_engine_gtest
            COMMAND modernime_personal_engine_gtest
        )
        set_tests_properties(modernime_personal_engine_gtest PROPERTIES
            RUN_SERIAL TRUE
        )
    elseif(MODERNIME_REQUIRE_FULL_TESTS)
        message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires Google Test (gtest and gtest_main), but it was not found.")
    endif()
endif()

find_package(Threads REQUIRED)
if(NOT MODERNIME_SIMULATE_MISSING_BENCHMARK)
    find_path(BENCHMARK_INCLUDE_DIR
        NAMES benchmark/benchmark.h
        HINTS
            "$ENV{HOME}/.local/include"
            "/home/$ENV{USER}/.local/include"
    )
    find_library(BENCHMARK_LIBRARY
        NAMES benchmark
        HINTS
            "$ENV{HOME}/.local/lib"
            "/home/$ENV{USER}/.local/lib"
    )
endif()
if(BENCHMARK_INCLUDE_DIR AND BENCHMARK_LIBRARY)
    get_filename_component(BENCHMARK_LIB_DIR "${BENCHMARK_LIBRARY}" DIRECTORY)
    add_executable(modernime_personal_engine_benchmark
        core/personal_engine_benchmark.cpp
    )
    target_include_directories(modernime_personal_engine_benchmark PRIVATE
        ${BENCHMARK_INCLUDE_DIR}
    )
    target_link_libraries(modernime_personal_engine_benchmark PRIVATE
        modernime_core
        ${BENCHMARK_LIBRARY}
        Threads::Threads
    )
    if(MODERNIME_BUILD_LIBIME_PINYIN)
        target_link_libraries(modernime_personal_engine_benchmark PRIVATE
            modernime_pinyin
        )
        target_compile_definitions(modernime_personal_engine_benchmark PRIVATE
            MODERNIME_HAS_PINYIN_PROVIDER=1
        )
    endif()
    if(NOT DEFINED MODERNIME_ENABLE_PERFORMANCE_GATE)
        if(CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
            set(MODERNIME_ENABLE_PERFORMANCE_GATE ON)
        else()
            set(MODERNIME_ENABLE_PERFORMANCE_GATE OFF)
        endif()
    endif()
    if(MODERNIME_ENABLE_PERFORMANCE_GATE)
        target_compile_definitions(modernime_personal_engine_benchmark PRIVATE
            MODERNIME_PERFORMANCE_GATE_ACTIVE=1
        )
    endif()
    set_target_properties(modernime_personal_engine_benchmark PROPERTIES
        BUILD_RPATH "${BENCHMARK_LIB_DIR}"
        INSTALL_RPATH "${BENCHMARK_LIB_DIR}"
    )
    add_test(NAME modernime_personal_engine_benchmark
        COMMAND modernime_personal_engine_benchmark --benchmark_min_time=0.02
    )
    # Wall-clock P99 is a release gate; competing build/install tests can
    # preempt the measured thread and turn scheduling delay into a regression.
    set_tests_properties(modernime_personal_engine_benchmark PROPERTIES
        RUN_SERIAL TRUE
    )
elseif(MODERNIME_REQUIRE_FULL_TESTS)
    message(FATAL_ERROR "MODERNIME_REQUIRE_FULL_TESTS=ON requires Google Benchmark, but it was not found.")
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

if(MODERNIME_BUILD_FCITX5)
    find_package(Fcitx5Core REQUIRED)

    add_executable(modernime_clipboard_trigger_tests
        adapter/clipboard_trigger_test.cpp
        ../adapter/fcitx5/src/engine.cpp
        ../adapter/fcitx5/src/inputmethod.cpp
    )
    target_include_directories(modernime_clipboard_trigger_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
    )
    target_link_libraries(modernime_clipboard_trigger_tests
        PRIVATE modernime_core Fcitx5::Core Fcitx5::Utils
    )
    add_test(NAME modernime_clipboard_trigger
        COMMAND modernime_clipboard_trigger_tests
    )

    add_executable(modernime_fcitx5_candidate_list_tests
        adapter/fcitx5_candidate_list_test.cpp
    )
    target_link_libraries(modernime_fcitx5_candidate_list_tests
        PRIVATE Fcitx5::Core
    )
    add_test(NAME modernime_fcitx5_candidate_list
        COMMAND modernime_fcitx5_candidate_list_tests
    )

    add_executable(modernime_install_discovery_tests adapter/install_discovery_test.cpp)
    target_include_directories(modernime_install_discovery_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include)
    target_link_libraries(modernime_install_discovery_tests PRIVATE Fcitx5::Utils)
    target_compile_features(modernime_install_discovery_tests PRIVATE cxx_std_20)
    add_test(NAME modernime_install_discovery COMMAND modernime_install_discovery_tests)

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
    if(MODERNIME_BUILD_LIBIME_PINYIN)
        target_compile_definitions(modernime_fcitx5_input_panel_tests PRIVATE
            MODERNIME_HAS_LIBIME_PINYIN=1
        )
        target_link_libraries(modernime_fcitx5_input_panel_tests PRIVATE modernime_pinyin)
    endif()
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

    find_package(Fcitx5Config REQUIRED)

    add_library(config_fault_hooks SHARED adapter/config_fault_hooks.c)
    target_link_libraries(config_fault_hooks PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(config_fault_hooks PROPERTIES
        PREFIX ""
        OUTPUT_NAME "config_fault_hooks"
    )

    add_executable(modernime_adapter_recovery_tests
        adapter/adapter_recovery_test.cpp
        ../adapter/fcitx5/src/inputmethod.cpp
        ../adapter/fcitx5/src/engine.cpp
    )
    target_include_directories(modernime_adapter_recovery_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
    )
    target_link_libraries(modernime_adapter_recovery_tests
        PRIVATE modernime_core Fcitx5::Core Fcitx5::Config Fcitx5::Utils SQLite::SQLite3 LMDB::LMDB ${CMAKE_DL_LIBS} pthread
    )
    if(MODERNIME_BUILD_LIBIME_PINYIN)
        target_compile_definitions(modernime_adapter_recovery_tests PRIVATE
            MODERNIME_HAS_LIBIME_PINYIN=1
        )
        target_link_libraries(modernime_adapter_recovery_tests PRIVATE modernime_pinyin)
    endif()
    add_dependencies(modernime_adapter_recovery_tests config_fault_hooks)
    add_test(NAME modernime_adapter_recovery COMMAND modernime_adapter_recovery_tests)
    set_tests_properties(modernime_adapter_recovery PROPERTIES
        ENVIRONMENT "MODERNIME_FAULT_HOOKS_LIB=$<TARGET_FILE:config_fault_hooks>"
        TIMEOUT 120
    )
endif()

if(MODERNIME_BUILD_UI_LAYOUT_TESTS)
    add_executable(modernime_ui_tests ui/candidate_bar_layout_test.cpp)
    target_include_directories(modernime_ui_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
    )
    target_link_libraries(modernime_ui_tests PRIVATE modernime_core)
    target_sources(modernime_ui_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_layout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_pagination.cpp
    )
    add_test(NAME modernime_candidate_bar_layout COMMAND modernime_ui_tests)
    if(NOT TARGET modernime_candidate_bar_layout_tests)
        add_custom_target(modernime_candidate_bar_layout_tests
            DEPENDS modernime_ui_tests
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                $<TARGET_FILE:modernime_ui_tests>
                $<TARGET_FILE_DIR:modernime_ui_tests>/modernime_candidate_bar_layout_tests
            COMMENT "Create alias symlink modernime_candidate_bar_layout_tests -> modernime_ui_tests"
        )
    endif()

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
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_pagination.cpp
    )
    target_link_libraries(modernime_candidate_bar_renderer_tests PRIVATE modernime_core)
    add_test(NAME modernime_candidate_bar_renderer COMMAND modernime_candidate_bar_renderer_tests)

    add_executable(modernime_candidate_reading_hint_tests ui/candidate_reading_hint_test.cpp)
    target_include_directories(modernime_candidate_reading_hint_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
    )
    target_sources(modernime_candidate_reading_hint_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_renderer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_bar_layout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/src/candidate_pagination.cpp
    )
    target_link_libraries(modernime_candidate_reading_hint_tests PRIVATE modernime_core)
    add_test(NAME modernime_candidate_reading_hint COMMAND modernime_candidate_reading_hint_tests)

    add_executable(modernime_cairo_surface_tests ui/cairo_surface_test.cpp)
    target_include_directories(modernime_cairo_surface_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
    )
    target_link_libraries(modernime_cairo_surface_tests PRIVATE modernime_ui_layout)
    add_test(NAME modernime_cairo_surface COMMAND modernime_cairo_surface_tests)

    if(MODERNIME_BUILD_FCITX5)
        pkg_check_modules(GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
        pkg_check_modules(PANGOCAIRO QUIET IMPORTED_TARGET pangocairo)
        if(GTK3_FOUND AND PANGOCAIRO_FOUND)
            pkg_check_modules(AYATANA_APPINDICATOR QUIET IMPORTED_TARGET ayatana-appindicator3-0.1)
            set(UI_TEST_APPINDICATOR_TARGET "")
            set(UI_TEST_APPINDICATOR_DEF "")
            if(AYATANA_APPINDICATOR_FOUND)
                set(UI_TEST_APPINDICATOR_TARGET PkgConfig::AYATANA_APPINDICATOR)
                set(UI_TEST_APPINDICATOR_DEF "MODERNIME_USE_AYATANA_APPINDICATOR")
            else()
                pkg_check_modules(APPINDICATOR QUIET IMPORTED_TARGET appindicator3-0.1)
                if(APPINDICATOR_FOUND)
                    set(UI_TEST_APPINDICATOR_TARGET PkgConfig::APPINDICATOR)
                    set(UI_TEST_APPINDICATOR_DEF "MODERNIME_USE_APPINDICATOR")
                endif()
            endif()

            add_executable(modernime_ui_addon_tests
                ui/ui_addon_test.cpp
                ../ui/fcitx5/src/ui_addon.cpp
            )
            target_include_directories(modernime_ui_addon_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../ui/fcitx5/include
                ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/settings/include
            )
            target_link_libraries(modernime_ui_addon_tests PRIVATE
                modernime_ui_layout
                modernime_core
                Fcitx5::Core
                Fcitx5::Config
                Fcitx5::Utils
                PkgConfig::GTK3
                PkgConfig::PANGOCAIRO
                SQLite::SQLite3
                LMDB::LMDB
            )
            if(UI_TEST_APPINDICATOR_TARGET)
                target_link_libraries(modernime_ui_addon_tests PRIVATE ${UI_TEST_APPINDICATOR_TARGET})
                target_compile_definitions(modernime_ui_addon_tests PRIVATE ${UI_TEST_APPINDICATOR_DEF})
            endif()
            add_test(NAME modernime_ui_addon COMMAND modernime_ui_addon_tests)
            set_tests_properties(modernime_ui_addon PROPERTIES
                RESOURCE_LOCK modernime_gtk_display
                TIMEOUT 20
                SKIP_RETURN_CODE 77
            )
        endif()
    endif()
endif()

if(BASH_EXECUTABLE)
    add_test(NAME modernime_repository_contract
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/repository_contract_test.sh
            ${CMAKE_BINARY_DIR}
    )
    add_test(NAME modernime_pinyin_knowledge_data
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/pinyin_knowledge_data_test.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/../data/pinyin/modernime-knowledge.tsv
            ${CMAKE_CURRENT_SOURCE_DIR}/../data/pinyin/modernime-knowledge.raw
            ${CMAKE_CURRENT_SOURCE_DIR}/../data/pinyin/modernime-hotwords.raw
    )
    add_test(NAME modernime_install_runtime
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_runtime_test.sh
    )
    add_test(NAME modernime_install_environment
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_environment_test.sh
    )
    add_test(NAME modernime_session_init
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/session_init_test.sh
    )
    add_test(NAME modernime_settings_install
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/settings/settings_install_test.sh
    )
    add_test(NAME modernime_install_transaction
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_transaction_test.sh
    )
    set_tests_properties(modernime_install_transaction PROPERTIES
        RESOURCE_LOCK modernime_install_transaction
    )
    add_test(NAME modernime_install_rollback_concurrent_writes
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_rollback_concurrent_writes_test.sh
    )
    set_tests_properties(modernime_install_rollback_concurrent_writes PROPERTIES
        RESOURCE_LOCK modernime_install_transaction
    )
    add_test(NAME modernime_install_lifecycle_prep
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_lifecycle_prep_test.sh
    )
    set_tests_properties(modernime_install_lifecycle_prep PROPERTIES
        RESOURCE_LOCK modernime_install_transaction
    )
    add_test(NAME modernime_install_channel_collision
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/install_channel_collision_test.sh
    )
    add_test(NAME modernime_verify_release_install_upgrade
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/verify_release_install_upgrade.sh
    )
    add_test(NAME modernime_upgrade_data_protection
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/upgrade_data_protection_test.sh
    )
    add_test(NAME modernime_cleanse_guard
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cleanse_guard_test.sh
    )
    add_test(NAME modernime_verify_package_repo_gate
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/verify_package_repo_gate.sh
    )
    set_tests_properties(modernime_verify_package_repo_gate modernime_verify_release_install_upgrade PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 300
    )
endif()
