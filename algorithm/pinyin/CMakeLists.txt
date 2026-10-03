find_package(LibIMECore REQUIRED)
find_package(LibIMEPinyin REQUIRED)

add_library(modernime_pinyin STATIC
    src/candidate_mixer.cpp
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

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES LibIME::Pinyin)
check_cxx_source_compiles("
#include <libime/pinyin/pinyinencoder.h>
int main() {
    auto f = libime::PinyinFuzzyFlag::CommonTypo;
    (void)f;
    return 0;
}
" MODERNIME_HAS_PINYIN_COMMON_TYPO)

if(MODERNIME_HAS_PINYIN_COMMON_TYPO)
    target_compile_definitions(modernime_pinyin PRIVATE MODERNIME_HAS_PINYIN_COMMON_TYPO=1)
endif()

if(MODERNIME_BUILD_LIBIME_PINYIN)
    add_dependencies(modernime_pinyin
        modernime_pinyin_knowledge_dictionary
        modernime_pinyin_hotwords_dictionary)
    target_compile_definitions(modernime_pinyin
        PUBLIC
            "MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY=\"${CMAKE_BINARY_DIR}/data/pinyin/modernime-knowledge.dict\""
            "MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY=\"${CMAKE_INSTALL_FULL_DATADIR}/modernime/pinyin/modernime-knowledge.dict\""
            "MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY=\"${CMAKE_BINARY_DIR}/data/pinyin/modernime-hotwords.dict\""
            "MODERNIME_PINYIN_HOTWORDS_INSTALL_BINARY=\"${CMAKE_INSTALL_FULL_DATADIR}/modernime/pinyin/modernime-hotwords.dict\""
            "MODERNIME_ENGLISH_DICT_SOURCE_BINARY=\"${CMAKE_SOURCE_DIR}/data/pinyin/modernime-english-dict.bin\""
            "MODERNIME_ENGLISH_DICT_INSTALL_BINARY=\"${CMAKE_INSTALL_FULL_DATADIR}/modernime/pinyin/modernime-english-dict.bin\""
    )
endif()
