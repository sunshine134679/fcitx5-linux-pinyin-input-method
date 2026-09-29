find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

add_library(modernime_ui_layout STATIC
    src/candidate_bar_layout.cpp
    src/candidate_pagination.cpp
    src/candidate_bar_renderer.cpp
    src/cairo_render_surface.cpp
)
set_target_properties(modernime_ui_layout PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(modernime_ui_layout PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(modernime_ui_layout PUBLIC modernime_core PkgConfig::PANGOCAIRO)
modernime_enable_warnings(modernime_ui_layout)

if(MODERNIME_BUILD_FCITX5)
    find_package(Fcitx5Core REQUIRED)
    find_package(Fcitx5Utils REQUIRED)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

    # 托盘指示器支持：优先检测 Ayatana AppIndicator，次选标准 AppIndicator，若均无则平滑降级
    pkg_check_modules(AYATANA_APPINDICATOR QUIET IMPORTED_TARGET ayatana-appindicator3-0.1)
    if(AYATANA_APPINDICATOR_FOUND)
        set(MODERNIME_APPINDICATOR_TARGET PkgConfig::AYATANA_APPINDICATOR)
        set(MODERNIME_APPINDICATOR_DEF "MODERNIME_USE_AYATANA_APPINDICATOR")
    else()
        pkg_check_modules(APPINDICATOR QUIET IMPORTED_TARGET appindicator3-0.1)
        if(APPINDICATOR_FOUND)
            set(MODERNIME_APPINDICATOR_TARGET PkgConfig::APPINDICATOR)
            set(MODERNIME_APPINDICATOR_DEF "MODERNIME_USE_APPINDICATOR")
        else()
            set(MODERNIME_APPINDICATOR_TARGET "")
            set(MODERNIME_APPINDICATOR_DEF "")
        endif()
    endif()

    add_library(modernime_ui MODULE
        src/ui_addon.cpp
    )
    target_include_directories(modernime_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../adapter/fcitx5/include
    )
    target_link_libraries(modernime_ui PRIVATE
        modernime_ui_layout
        Fcitx5::Core
        Fcitx5::Utils
        PkgConfig::GTK3
    )
    if(MODERNIME_APPINDICATOR_TARGET)
        target_link_libraries(modernime_ui PRIVATE ${MODERNIME_APPINDICATOR_TARGET})
        target_compile_definitions(modernime_ui PRIVATE ${MODERNIME_APPINDICATOR_DEF})
    endif()

    target_compile_features(modernime_ui PRIVATE cxx_std_20)
    modernime_enable_warnings(modernime_ui)
    set_target_properties(modernime_ui PROPERTIES PREFIX "")

    install(TARGETS modernime_ui LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../config/addon/modernime-ui.conf
            DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
endif()

