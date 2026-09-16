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

install(FILES "${MODERNIME_PINYIN_HOTWORDS_BINARY}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/pinyin")
