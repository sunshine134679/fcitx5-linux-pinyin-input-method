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
