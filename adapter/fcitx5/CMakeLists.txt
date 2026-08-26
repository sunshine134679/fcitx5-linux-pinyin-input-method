find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)

add_library(modernime_fcitx5 MODULE
    src/addon.cpp
    src/engine.cpp
    src/inputmethod.cpp
)

target_include_directories(modernime_fcitx5
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(MODERNIME_BUILD_LIBIME_PINYIN)
    target_compile_definitions(modernime_fcitx5 PRIVATE MODERNIME_HAS_LIBIME_PINYIN)
    target_include_directories(modernime_fcitx5 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../algorithm/pinyin/include
    )
endif()
target_link_libraries(modernime_fcitx5
    PRIVATE
        modernime_core
        Fcitx5::Core
        Fcitx5::Utils
)
if(MODERNIME_BUILD_LIBIME_PINYIN)
    target_link_libraries(modernime_fcitx5 PRIVATE modernime_pinyin)
endif()
target_compile_features(modernime_fcitx5 PRIVATE cxx_std_20)
modernime_enable_warnings(modernime_fcitx5)
set_target_properties(modernime_fcitx5 PROPERTIES PREFIX "")

install(TARGETS modernime_fcitx5 LIBRARY DESTINATION lib/fcitx5)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/addon/modernime.conf
        DESTINATION share/fcitx5/addon)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/inputmethod/modernime.conf
        DESTINATION share/fcitx5/inputmethod)
