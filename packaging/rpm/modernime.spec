Name:           modernime
Version:        0.1.9
Release:        1%{?dist}
Summary:        Modern Fcitx5 Pinyin Input Method for Linux Desktops
# ModernIME engine, UI addon, and settings client are LGPL-2.1-or-later;
# bundled derived pinyin knowledge and English definition dictionaries are MIT.
License:        LGPL-2.1-or-later AND MIT
URL:            https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ >= 10
BuildRequires:  cmake >= 3.24
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  sqlite-devel >= 3.31.0
BuildRequires:  lmdb-devel >= 0.9.24
BuildRequires:  boost-devel >= 1.61
BuildRequires:  fcitx5-devel >= 5.0.14
BuildRequires:  libime-devel >= 1.0.11
BuildRequires:  gtk3-devel >= 3.24.0
BuildRequires:  pango-devel >= 1.44.0
BuildRequires:  cairo-devel >= 1.16.0
BuildRequires:  libappindicator-gtk3-devel

Requires:       fcitx5 >= 5.0.14
Requires:       libime >= 1.0.11
Requires:       gtk3 >= 3.24.0
Requires:       pango >= 1.44.0
Requires:       cairo >= 1.16.0
Requires:       sqlite >= 3.31.0
Requires:       lmdb-libs >= 0.9.24

%description
ModernIME is a modern Pinyin input method engine for Fcitx5 on Linux desktops,
featuring smart candidate ranking, error correction, offline dictionaries,
Bento-designed GTK3 settings client, and comprehensive character governance.

%prep
%autosetup -p1

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DMODERNIME_PACKAGE_CHANNEL=rpm \
    -DMODERNIME_BUILD_FCITX5=ON \
    -DMODERNIME_BUILD_LIBIME_PINYIN=ON \
    -DMODERNIME_BUILD_SETTINGS=ON \
    -DMODERNIME_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%pre
if [ -f %{_datadir}/modernime/install-channel.txt ]; then
    existing_channel=$(grep -E '^channel=' %{_datadir}/modernime/install-channel.txt 2>/dev/null | head -n 1 | cut -d= -f2 || true)
    if [ "$existing_channel" = "script" ] && [ "${MODERNIME_MIGRATE_FROM_SCRIPT:-0}" != "1" ]; then
        echo "Error: Detected an existing script-based ModernIME installation in /usr (channel=script)." >&2
        exit 1
    fi
fi
if [ "${1:-1}" -ge 2 ] && [ -x %{_bindir}/modernime-data-guard ]; then
    %{_bindir}/modernime-data-guard --pre-upgrade
fi

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
%license LICENSE
%{_bindir}/modernime-settings
%{_bindir}/modernime-session-init
%{_bindir}/modernime-data-guard
%{_bindir}/modernime-migrate-channel
%{_libdir}/fcitx5/modernime_fcitx5.so
%{_libdir}/fcitx5/modernime_ui.so
%{_datadir}/fcitx5/addon/modernime.conf
%{_datadir}/fcitx5/addon/modernime-ui.conf
%{_datadir}/fcitx5/inputmethod/modernime.conf
%{_datadir}/modernime/install-channel.txt
%{_datadir}/modernime/pinyin/modernime-knowledge.dict
%{_datadir}/modernime/pinyin/modernime-hotwords.dict
%{_datadir}/modernime/pinyin/modernime-english-dict.bin
%{_datadir}/doc/modernime/LICENSE
%{_datadir}/doc/modernime/copyright
%{_datadir}/doc/modernime/THIRD_PARTY_NOTICES.md
%{_datadir}/doc/modernime/SOURCES.json
%{_datadir}/applications/modernime-settings.desktop

%changelog
* Mon Oct 12 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.9-1
- Formalize online APT and DNF package repository upgrade architecture
- Add signed static repository pipelines with multi-version retention and rollback support
- Fix RPM header digests (PAYLOADDIGEST, SHA1/SHA256/MD5) and OpenPGP package signing
- Support GPG passphrase (MODERNIME_GPG_PASSPHRASE) and dual-key rotation transitions
- Decouple package manager long operations timeout (300s) from short queries in settings client
- Add controlled script-to-package migration tool (modernime-migrate-channel) with transactional rollback

* Mon Oct 12 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.8-1
- Add LMDB personalization storage engine with lossless SQLite migration and process-wide shared handles
- Implement commit consistency, exponential half-life decay, surrounding text context affinity, and sensitive context guards
- Expand Chinese lexicon with names, daily phrases, and domain terms with polyphonic verification
- Display English definitions as candidate annotations and unify Chinese/English arbitration
- Harden online WAL/LMDB backup, staged binary payload rollback, and release upgrade lifecycle gate

* Sun Oct 11 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.7-1
- Preserve user integration files and track session profile ownership
- Share source and portable installation lifecycle with relocatable data lookup
- Fix input toggles, numeric macros, candidate ordering and clipboard synchronization
- Refresh cleared WAL learning state without blocking input or restoring old writes

* Sun Oct 04 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.6-1
- Prioritize common high-frequency Chinese vocabulary (e.g. name->那么, like->立刻)
- Protect core Chinese lexical candidates against accidental English learning hijacking

* Sun Oct 04 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.5-1
- Support game-like keyboard shortcut recording in settings
- Multi-distribution dynamic language model and dictionary path probing
- Support RPM and 64-bit multiarch library directory layout (lib64)
- Add system-wide root installation and session integration
- Update build dependencies with libappindicator-gtk3-devel

* Sun Sep 29 2026 ModernIME Maintainers <sunshine134679@users.noreply.github.com> - 0.1.0-1
- Initial release for openEuler and RPM distributions
