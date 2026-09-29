# ModernIME CPack Configuration
# Enables generating DEB, RPM, and TGZ packages

set(CPACK_PACKAGE_NAME "modernime")
set(CPACK_PACKAGE_VENDOR "ModernIME Community")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern Fcitx5 Pinyin Input Method for Linux Desktops")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "ModernIME Maintainers <sunshine134679@users.noreply.github.com>")

# Deb generator configuration
set(CPACK_DEBIAN_PACKAGE_NAME "modernime")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ModernIME Maintainers <sunshine134679@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method")
find_program(MODERNIME_DPKG_SHLIBDEPS dpkg-shlibdeps)
if(MODERNIME_DPKG_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
else()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "fcitx5, libgtk-3-0, libsqlite3-0")
endif()
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fcitx5-chinese-addons")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "fcitx5-config-qt")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
    )
endif()

# RPM generator configuration (openEuler / Fedora / RHEL)
set(CPACK_RPM_PACKAGE_NAME "modernime")
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-2.1-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_URL "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method")
set(CPACK_RPM_PACKAGE_AUTOREQPROV "yes")
set(CPACK_RPM_PACKAGE_REQUIRES "fcitx5, libime, gtk3")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
    set(CPACK_RPM_PACKAGE_POST_INSTALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
    set(CPACK_RPM_PACKAGE_POST_UNINSTALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun.sh")
endif()

# Archive naming
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

include(CPack)
