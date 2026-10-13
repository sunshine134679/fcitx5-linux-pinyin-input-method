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
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
# Explicit lower bounds for core runtime frameworks alongside dpkg-shlibdeps SONAME resolution.
set(MODERNIME_DEB_BASE_DEPENDS
    "fcitx5 (>= 5.0.14), libime-bin (>= 1.0.11) | libimecore0 (>= 1.0.11) | libimecore0t64 (>= 1.0.11), libgtk-3-0 (>= 3.24.0) | libgtk-3-0t64 (>= 3.24.0), libpango-1.0-0 (>= 1.44.0), libcairo2 (>= 1.16.0), libsqlite3-0 (>= 3.31.0), liblmdb0 (>= 0.9.24), python3 (>= 3.10), coreutils"
)
find_program(MODERNIME_DPKG_SHLIBDEPS dpkg-shlibdeps)
if(MODERNIME_DPKG_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "${MODERNIME_DEB_BASE_DEPENDS}")
else()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "${MODERNIME_DEB_BASE_DEPENDS}, libfcitx5core7 (>= 5.0.14), libfcitx5utils2 (>= 5.0.14), libimecore0 (>= 1.0.11), libimepinyin0 (>= 1.0.11)"
    )
endif()
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fcitx5-chinese-addons, libime-data, libime-data-language-model")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "fcitx5-config-qt")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/preinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
    )
endif()

# RPM generator configuration (openEuler / Fedora / RHEL)
# Code is LGPL-2.1-or-later; bundled derived dictionary data is MIT (see THIRD_PARTY_NOTICES.md).
set(CPACK_RPM_PACKAGE_NAME "modernime")
set(CPACK_RPM_PACKAGE_LICENSE "LGPL-2.1-or-later AND MIT")
set(CPACK_RPM_PACKAGE_RELEASE "1.oe2409")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_URL "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method")
set(CPACK_RPM_PACKAGE_AUTOREQPROV "yes")
set(CPACK_RPM_COMPRESSION_TYPE "gzip")
set(CPACK_RPM_SPEC_MORE_DEFINE "%define _build_id_links none")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/lib64/fcitx5"
    "/usr/lib/fcitx5"
    "/usr/lib/x86_64-linux-gnu"
    "/usr/lib/x86_64-linux-gnu/fcitx5"
    "/usr/share/applications"
    "/usr/share/fcitx5"
    "/usr/share/fcitx5/addon"
    "/usr/share/fcitx5/inputmethod"
    "/usr/share/icons"
    "/usr/share/icons/hicolor"
    "/usr/share/icons/hicolor/scalable"
    "/usr/share/icons/hicolor/scalable/apps"
)
set(CPACK_RPM_PACKAGE_REQUIRES
    "fcitx5 >= 5.0.14, libime >= 1.0.11, gtk3 >= 3.24.0, pango >= 1.44.0, cairo >= 1.16.0, sqlite >= 3.31.0, sqlite-libs >= 3.37.0, lmdb-libs >= 0.9.24, python3 >= 3.10, coreutils, desktop-file-utils, hicolor-icon-theme"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
    set(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preinst.sh")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun.sh")
    set(CPACK_RPM_PACKAGE_PRE_INSTALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/preinst.sh")
    set(CPACK_RPM_PACKAGE_POST_INSTALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postinst.sh")
    set(CPACK_RPM_PACKAGE_POST_UNINSTALL "${CMAKE_CURRENT_SOURCE_DIR}/packaging/rpm/postun.sh")
endif()

# Archive naming
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

include(CPack)
