add_library(modernime_core STATIC
    src/candidate_model.cpp
    src/input_state.cpp
    src/candidate_ranker.cpp
    src/pinyin_match.cpp
)
target_include_directories(modernime_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(modernime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
modernime_enable_warnings(modernime_core)
