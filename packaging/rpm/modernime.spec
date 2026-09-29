Name:           modernime
Version:        0.1.0
Release:        1%{?dist}
Summary:        Modern Fcitx5 Pinyin Input Method for Linux Desktops
License:        LGPL-2.1-or-later
URL:            https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ >= 10
BuildRequires:  cmake >= 3.24
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  sqlite-devel
BuildRequires:  boost-devel
BuildRequires:  fcitx5-devel
BuildRequires:  libime-devel
BuildRequires:  gtk3-devel
BuildRequires:  pango-devel
BuildRequires:  cairo-devel

Requires:       fcitx5
Requires:       libime
Requires:       gtk3
Requires:       sqlite

%description
ModernIME is a modern Pinyin input method engine for Fcitx5 on Linux desktops,
featuring smart candidate ranking, error correction, offline dictionaries,
Bento-designed GTK3 settings client, and comprehensive character governance.

%prep
%autosetup -p1

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%post
if [ -x %{_bindir}/update-desktop-database ]; then
    %{_bindir}/update-desktop-database -q %{_datadir}/applications || true
fi
if command -v fcitx5-remote >/dev/null 2>&1; then
    fcitx5-remote -r >/dev/null 2>&1 || true
fi

%postun
if [ -x %{_bindir}/update-desktop-database ]; then
    %{_bindir}/update-desktop-database -q %{_datadir}/applications || true
fi
if command -v fcitx5-remote >/dev/null 2>&1; then
    fcitx5-remote -r >/dev/null 2>&1 || true
fi

%files
%{_bindir}/modernime-settings
%{_libdir}/fcitx5/modernime_fcitx5.so
%{_libdir}/fcitx5/modernime_ui.so
%{_datadir}/fcitx5/addon/modernime.conf
%{_datadir}/fcitx5/addon/modernime-ui.conf
%{_datadir}/fcitx5/inputmethod/modernime.conf
%{_datadir}/modernime/pinyin/modernime-knowledge.dict
%{_datadir}/modernime/pinyin/modernime-hotwords.dict
%{_datadir}/applications/modernime-settings.desktop

%changelog
* Sun Sep 29 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.0-1
- Initial release for openEuler and RPM distributions
