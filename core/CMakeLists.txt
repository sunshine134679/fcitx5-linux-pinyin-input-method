add_library(modernime_core STATIC
    src/candidate_model.cpp
    src/english_dictionary.cpp
    src/input_state.cpp
    src/candidate_ranker.cpp
    src/pinyin_match.cpp
    src/punctuation.cpp
    src/learning_snapshot.cpp
    src/learning_store.cpp
    src/learning_writer.cpp
    src/personal_engine.cpp
    src/lmdb_environment.cpp
    src/personal_engine_state.cpp
    src/async_clipboard_history.cpp
    src/clipboard_observation.cpp
    src/clipboard_history.cpp
    src/persistent_clipboard_history.cpp
    src/symbol_catalog.cpp
    src/calculator.cpp
    src/component_lookup.cpp
    src/settings.cpp
    src/english_definition_dictionary.cpp
    src/phrase_formation.cpp
    src/learning_policy.cpp
)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LMDB QUIET lmdb)
endif()
find_path(LMDB_INCLUDE_DIR
    NAMES lmdb.h
    HINTS
        ${LMDB_INCLUDE_DIRS}
    PATHS
        "$ENV{HOME}/.local/include"
        "/home/$ENV{USER}/.local/include"
)
find_library(LMDB_LIBRARY
    NAMES lmdb liblmdb.so liblmdb.so.0 liblmdb.a
    NAMES_PER_DIR
    HINTS
        ${LMDB_LIBRARY_DIRS}
    PATHS
        "$ENV{HOME}/.local/lib"
        "/home/$ENV{USER}/.local/lib"
)
if(NOT LMDB_INCLUDE_DIR OR NOT LMDB_LIBRARY)
    message(FATAL_ERROR "LMDB (liblmdb-dev / lmdb-devel) is required to build modernime_core")
endif()
if(NOT TARGET LMDB::LMDB)
    add_library(LMDB::LMDB UNKNOWN IMPORTED GLOBAL)
    set_target_properties(LMDB::LMDB PROPERTIES
        IMPORTED_LOCATION "${LMDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LMDB_INCLUDE_DIR}"
    )
endif()

target_include_directories(modernime_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(modernime_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(modernime_core PUBLIC SQLite::SQLite3 LMDB::LMDB Threads::Threads)
modernime_enable_warnings(modernime_core)

# Pre-generated and vendored: ordinary builds and runtime stay offline.
target_compile_definitions(modernime_core PRIVATE
    MODERNIME_COMPONENT_LOOKUP_PATH="${CMAKE_INSTALL_FULL_DATADIR}/modernime/components/component_lookup.tsv"
    MODERNIME_COMPONENT_LOOKUP_SOURCE_PATH="${PROJECT_SOURCE_DIR}/data/components/component_lookup.tsv")
install(FILES
    "${PROJECT_SOURCE_DIR}/data/components/component_lookup.tsv"
    "${PROJECT_SOURCE_DIR}/data/components/数据来源与许可.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/components")
install(FILES
    "${PROJECT_SOURCE_DIR}/data/components/upstream/chaizi.LICENSE"
    "${PROJECT_SOURCE_DIR}/data/components/upstream/pinyin.LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/components/upstream")
