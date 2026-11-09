find_program(LIBIME_PINYINDICT_EXECUTABLE
    NAMES libime_pinyindict
    REQUIRED
)

set(MODERNIME_PINYIN_KNOWLEDGE_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/modernime-knowledge.dict"
)

add_custom_command(
    OUTPUT "${MODERNIME_PINYIN_KNOWLEDGE_BINARY}"
    COMMAND "${LIBIME_PINYINDICT_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/modernime-knowledge.raw"
            "${MODERNIME_PINYIN_KNOWLEDGE_BINARY}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/modernime-knowledge.raw"
    COMMENT "Building ModernIME offline pinyin knowledge dictionary"
    VERBATIM
)

add_custom_target(modernime_pinyin_knowledge_dictionary
    ALL
    DEPENDS "${MODERNIME_PINYIN_KNOWLEDGE_BINARY}"
)

install(FILES "${MODERNIME_PINYIN_KNOWLEDGE_BINARY}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/pinyin")

# 网络热词表：独立于知识库主数据链，词条带正 cost，
# 让简拼（如 yyds）下热词越过系统词典组合排在候选前列。
set(MODERNIME_PINYIN_HOTWORDS_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/modernime-hotwords.dict"
)

add_custom_command(
    OUTPUT "${MODERNIME_PINYIN_HOTWORDS_BINARY}"
    COMMAND "${LIBIME_PINYINDICT_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/modernime-hotwords.raw"
            "${MODERNIME_PINYIN_HOTWORDS_BINARY}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/modernime-hotwords.raw"
    COMMENT "Building ModernIME internet hotwords dictionary"
    VERBATIM
)

add_custom_target(modernime_pinyin_hotwords_dictionary
    ALL
    DEPENDS "${MODERNIME_PINYIN_HOTWORDS_BINARY}"
)

# Keep the primary distro model unchanged. This independent, higher precision
# model supplies contextual evidence; its fixed source is available offline.
find_program(LIBIME_SLM_BUILD_BINARY_EXECUTABLE
    NAMES libime_slm_build_binary
    REQUIRED
)
set(MODERNIME_CONTEXT_MODEL_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/lm_sc.arpa-20260629.tar.zst")
file(SHA256 "${MODERNIME_CONTEXT_MODEL_SOURCE}" MODERNIME_CONTEXT_MODEL_SOURCE_SHA256)
if(NOT MODERNIME_CONTEXT_MODEL_SOURCE_SHA256 STREQUAL
        "06808333b9173e5374cf2cb5afc12d08f5625bf9abb536489cac376fc05f2e7f")
    message(FATAL_ERROR "ModernIME context model source checksum mismatch")
endif()
set(MODERNIME_CONTEXT_MODEL_BINARY
    "${CMAKE_CURRENT_BINARY_DIR}/modernime-context.lm")
add_custom_command(
    OUTPUT "${MODERNIME_CONTEXT_MODEL_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E tar xf "${MODERNIME_CONTEXT_MODEL_SOURCE}"
    COMMAND "${LIBIME_SLM_BUILD_BINARY_EXECUTABLE}"
            -s -a 22 -q 8 -S 256M trie lm_sc.arpa "${MODERNIME_CONTEXT_MODEL_BINARY}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f lm_sc.arpa
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${MODERNIME_CONTEXT_MODEL_SOURCE}"
    COMMENT "Building ModernIME offline context language model"
    VERBATIM
)
add_custom_target(modernime_pinyin_context_model
    ALL DEPENDS "${MODERNIME_CONTEXT_MODEL_BINARY}")
install(FILES "${MODERNIME_CONTEXT_MODEL_BINARY}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/pinyin")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/licenses/LGPL-2.1.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/modernime/licenses")

install(FILES "${MODERNIME_PINYIN_HOTWORDS_BINARY}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/pinyin")

set(MODERNIME_ENGLISH_DICT_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/modernime-english-dict.bin"
)

install(FILES "${MODERNIME_ENGLISH_DICT_SOURCE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/pinyin")

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/SOURCES.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/modernime"
)
