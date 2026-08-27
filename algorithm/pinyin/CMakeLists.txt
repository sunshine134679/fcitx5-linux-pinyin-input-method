find_package(LibIMECore REQUIRED)
find_package(LibIMEPinyin REQUIRED)

add_library(modernime_pinyin STATIC
    src/candidate_pipeline.cpp
    src/pinyin_candidate_provider.cpp
    src/user_dictionary.cpp
)
target_include_directories(modernime_pinyin
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(modernime_pinyin
    PUBLIC
        modernime_core
        LibIME::Core
        LibIME::Pinyin
)
target_compile_features(modernime_pinyin PUBLIC cxx_std_20)
set_target_properties(modernime_pinyin PROPERTIES POSITION_INDEPENDENT_CODE ON)
modernime_enable_warnings(modernime_pinyin)
