#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
readme="$project_root/README.md"

test -f "$readme"
grep -F 'git@github.com:sunshine134679/fcitx5-linux-pinyin-input-method.git' \
    "$readme" >/dev/null
grep -F './install.sh' "$readme" >/dev/null
grep -F 'modernime-settings' "$readme" >/dev/null
grep -F 'modernime-settings.desktop' "$readme" >/dev/null
grep -F './uninstall.sh' "$readme" >/dev/null

cpack_file="$project_root/cmake/ModernIMECPack.cmake"
test -f "$cpack_file"
grep -F 'set(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE' "$cpack_file" >/dev/null
grep -F 'set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE' "$cpack_file" >/dev/null
grep -F 'set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE' "$cpack_file" >/dev/null
grep -F 'set(CPACK_RPM_PACKAGE_LICENSE "LGPL-2.1-or-later AND MIT")' "$cpack_file" >/dev/null
grep -F 'fcitx5 (>= 5.0.14)' "$cpack_file" >/dev/null
grep -F 'libime-bin (>= 1.0.11) | libimecore0 (>= 1.0.11)' "$cpack_file" >/dev/null
grep -F 'libgtk-3-0 (>= 3.24.0) | libgtk-3-0t64 (>= 3.24.0)' "$cpack_file" >/dev/null
grep -F 'libsqlite3-0 (>= 3.31.0)' "$cpack_file" >/dev/null
grep -F 'liblmdb0 (>= 0.9.24)' "$cpack_file" >/dev/null
grep -F 'set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)' "$cpack_file" >/dev/null
grep -F 'set(CPACK_RPM_PACKAGE_AUTOREQPROV "yes")' "$cpack_file" >/dev/null

deb_preinst="$project_root/packaging/debian/preinst"
deb_postinst="$project_root/packaging/debian/postinst"
deb_postrm="$project_root/packaging/debian/postrm"
deb_copyright="$project_root/packaging/debian/copyright"
rpm_preinst="$project_root/packaging/rpm/preinst.sh"
rpm_postinst="$project_root/packaging/rpm/postinst.sh"
rpm_postun="$project_root/packaging/rpm/postun.sh"
rpm_spec="$project_root/packaging/rpm/modernime.spec"

for script_file in "$deb_preinst" "$deb_postinst" "$deb_postrm" "$rpm_preinst" "$rpm_postinst" "$rpm_postun"; do
    test -f "$script_file" && test -s "$script_file"
    bash -n "$script_file"
done

test -f "$project_root/LICENSE" && test -s "$project_root/LICENSE"
grep -F 'LGPL-2.1-or-later AND MIT' "$project_root/LICENSE" >/dev/null
test -f "$deb_copyright" && test -s "$deb_copyright"
grep -F 'License: LGPL-2.1+' "$deb_copyright" >/dev/null
grep -F 'License: MIT' "$deb_copyright" >/dev/null
grep -F 'LGPL-2.1-or-later AND MIT' "$project_root/data/pinyin/THIRD_PARTY_NOTICES.md" >/dev/null
grep -F '"package_spdx_license": "LGPL-2.1-or-later AND MIT"' "$project_root/data/pinyin/SOURCES.json" >/dev/null
grep -F 'License:        LGPL-2.1-or-later AND MIT' "$rpm_spec" >/dev/null
grep -F 'Requires:       fcitx5 >= 5.0.14' "$rpm_spec" >/dev/null
grep -F 'Requires:       sqlite >= 3.31.0' "$rpm_spec" >/dev/null
grep -F 'Requires:       lmdb-libs >= 0.9.24' "$rpm_spec" >/dev/null

# Verify no user home or ~/.config / ~/.local paths are installed by CMakeLists
if grep -En 'install\(.*(~|\$ENV\{HOME\}|\.config|\.local/share/modernime/(user_history|user_words|custom_words|user\.dict))' \
    "$project_root/CMakeLists.txt" "$project_root/data/pinyin/CMakeLists.txt" "$project_root/tools/settings/CMakeLists.txt"; then
    echo "User config or learning data must never be packaged into system packages" >&2
    exit 1
fi

binary_dir="${1:-}"
if [[ -n "$binary_dir" && -f "$binary_dir/CPackConfig.cmake" ]]; then
    grep -F 'set(CPACK_RPM_PRE_INSTALL_SCRIPT_FILE' "$binary_dir/CPackConfig.cmake" >/dev/null
    grep -F 'set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE' "$binary_dir/CPackConfig.cmake" >/dev/null
    grep -F 'set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE' "$binary_dir/CPackConfig.cmake" >/dev/null
fi
if [[ -n "$binary_dir" && -f "$binary_dir/cmake_install.cmake" ]]; then
    stage_check=$(mktemp -d "${TMPDIR:-/tmp}/modernime-contract-stage.XXXXXX")
    cmake --install "$binary_dir" --prefix "$stage_check" >/dev/null
    ext_dir="$stage_check/share/modernime/extensions/kimpanel@kde.org"
    test -f "$ext_dir/metadata.json"
    test -f "$ext_dir/extension.js"
    test -f "$ext_dir/schemas/gschemas.compiled"
    test -f "$ext_dir/locale/zh_CN/LC_MESSAGES/gnome-shell-extensions-kimpanel.mo"
    ext_count=$(find "$ext_dir" -type f | wc -l)
    if [[ "$ext_count" -ne 20 ]]; then
        echo "Expected exactly 20 kimpanel extension files in install staging, got $ext_count" >&2
        rm -rf -- "$stage_check"
        exit 1
    fi
    if find "$stage_check" \( -name "*mimosa*" -o -name "*v2c*" \) | grep -q .; then
        echo "Staging contains dirty or uncommitted files (.mimosa / .v2c)" >&2
        rm -rf -- "$stage_check"
        exit 1
    fi
    rm -rf -- "$stage_check"
fi

session_init="$project_root/scripts/modernime-session-init"
test -f "$session_init" && test -x "$session_init"
bash -n "$session_init"
grep -F 'modernime-session-init' "$project_root/tools/settings/CMakeLists.txt" >/dev/null
grep -F '%{_bindir}/modernime-session-init' "$project_root/packaging/rpm/modernime.spec" >/dev/null

for helper_script in "$project_root/scripts/modernime-migrate-channel" \
                     "$project_root/scripts/build_apt_repo.sh" \
                     "$project_root/scripts/build_rpm_repo.sh" \
                     "$project_root/scripts/resolve_release_target.sh"; do
    test -f "$helper_script" && test -x "$helper_script"
    bash -n "$helper_script"
done
test -f "$project_root/scripts/modernime-data-guard" && test -x "$project_root/scripts/modernime-data-guard"

release_wf="$project_root/.github/workflows/release.yml"
test -f "$release_wf"
grep -F 'required: true' "$release_wf" >/dev/null
grep -F 'scripts/resolve_release_target.sh' "$release_wf" >/dev/null
grep -F 'scripts/modernime-data-guard" --init-lmdb' "$release_wf" >/dev/null
test "$(grep -Fc 'ref: ${{ needs.resolve-target.outputs.source_sha }}' "$release_wf")" -eq 4
test "$(grep -Fc 'Product Source Commit SHA:' "$release_wf")" -eq 4
grep -F 'tag_name: ${{ needs.resolve-target.outputs.tag }}' "$release_wf" >/dev/null
if grep -F 'target_commitish' "$release_wf" >/dev/null; then
    echo "release.yml must not pass redundant target_commitish when publishing an existing tag" >&2
    exit 1
fi
grep -F 'name: Upload Signed Release Assets Bundle' "$release_wf" >/dev/null
grep -F 'name: release-assets' "$release_wf" >/dev/null
python3 - "$release_wf" <<'PYEOF'
import sys
from pathlib import Path
content = Path(sys.argv[1]).read_text(encoding="utf-8")
idx_sign = content.index("- name: Verify Production-Signed Release RPM & Pages Byte Identity")
idx_sum = content.index("- name: Compute Checksums")
idx_upload = content.index("- name: Upload Signed Release Assets Bundle")
idx_pages = content.index("- name: Configure GitHub Pages")
idx_release = content.index("- name: Create GitHub Release")
assert idx_sign < idx_sum < idx_upload < idx_pages < idx_release, (
    f"Invalid step order: sign={idx_sign}, sum={idx_sum}, upload={idx_upload}, pages={idx_pages}, release={idx_release}"
)
PYEOF

test -f "$project_root/packaging/apt/modernime.sources.in"
grep -F 'Signed-By: /usr/share/keyrings/modernime-archive-keyring.gpg' "$project_root/packaging/apt/modernime.sources.in" >/dev/null
test -f "$project_root/packaging/apt/99-modernime.pref"
test -f "$project_root/packaging/rpm/modernime-openeuler.repo.in"
grep -F 'gpgcheck=1' "$project_root/packaging/rpm/modernime-openeuler.repo.in" >/dev/null
grep -F 'repo_gpgcheck=1' "$project_root/packaging/rpm/modernime-openeuler.repo.in" >/dev/null

grep -F '/etc/apt/sources.list.d/modernime.sources' "$readme" >/dev/null
grep -F '/usr/share/keyrings/modernime-archive-keyring.gpg' "$readme" >/dev/null
grep -F '/etc/yum.repos.d/modernime-openeuler-24.09.repo' "$readme" >/dev/null
grep -F 'modernime-migrate-channel' "$readme" >/dev/null
