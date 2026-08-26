add_executable(modernime_core_tests core/foundation_test.cpp)
target_link_libraries(modernime_core_tests PRIVATE modernime_core)
add_test(NAME modernime_core_foundation COMMAND modernime_core_tests)

add_executable(modernime_input_state_tests core/input_state_test.cpp)
target_link_libraries(modernime_input_state_tests PRIVATE modernime_core)
add_test(NAME modernime_input_state COMMAND modernime_input_state_tests)

add_executable(modernime_adapter_tests adapter/engine_state_test.cpp)
target_include_directories(modernime_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/include
)
target_sources(modernime_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../adapter/fcitx5/src/engine.cpp
)
target_link_libraries(modernime_adapter_tests PRIVATE modernime_core)
add_test(NAME modernime_engine_state COMMAND modernime_adapter_tests)
