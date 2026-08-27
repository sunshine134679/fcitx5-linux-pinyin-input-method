add_library(modernime_core STATIC
    src/candidate_model.cpp
    src/input_state.cpp
    src/candidate_ranker.cpp
    src/pinyin_match.cpp
    src/learning_snapshot.cpp
    src/learning_store.cpp
    src/learning_writer.cpp
)
find_package(SQLite3 REQUIRED)
target_include_directories(modernime_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(modernime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(modernime_core PUBLIC SQLite::SQLite3)
modernime_enable_warnings(modernime_core)
