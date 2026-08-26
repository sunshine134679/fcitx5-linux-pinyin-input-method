find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)

add_library(modernime_fcitx5 MODULE
    src/addon.cpp
    src/inputmethod.cpp
)

target_include_directories(modernime_fcitx5
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(modernime_fcitx5
    PRIVATE
        modernime_core
        Fcitx5::Core
        Fcitx5::Utils
)
target_compile_features(modernime_fcitx5 PRIVATE cxx_std_20)
modernime_enable_warnings(modernime_fcitx5)
set_target_properties(modernime_fcitx5 PROPERTIES PREFIX "")
