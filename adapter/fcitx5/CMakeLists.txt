find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)
find_package(Threads REQUIRED)

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
        ${CMAKE_DL_LIBS}
)
if(MODERNIME_BUILD_LIBIME_PINYIN)
    target_link_libraries(modernime_fcitx5 PRIVATE modernime_pinyin)
endif()
target_compile_features(modernime_fcitx5 PRIVATE cxx_std_20)
modernime_enable_warnings(modernime_fcitx5)
set_target_properties(modernime_fcitx5 PROPERTIES PREFIX "")

install(TARGETS modernime_fcitx5 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/addon/modernime.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/inputmethod/modernime.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod)


if(TARGET modernime_pinyin)
    add_library(modernime_fcitx5_backend STATIC src/backend_service.cpp)
    set_target_properties(modernime_fcitx5_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(modernime_fcitx5_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(modernime_fcitx5_backend PUBLIC modernime_pinyin Fcitx5::Utils Threads::Threads)
    modernime_enable_warnings(modernime_fcitx5_backend)
    target_link_libraries(modernime_fcitx5 PRIVATE modernime_fcitx5_backend)
endif()
