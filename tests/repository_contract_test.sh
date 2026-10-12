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
grep -F 'set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE' "$cpack_file" >/dev/null
grep -F 'set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE' "$cpack_file" >/dev/null

rpm_postinst="$project_root/packaging/rpm/postinst.sh"
rpm_postun="$project_root/packaging/rpm/postun.sh"
test -f "$rpm_postinst" && test -s "$rpm_postinst"
test -f "$rpm_postun" && test -s "$rpm_postun"
bash -n "$rpm_postinst"
bash -n "$rpm_postun"
binary_dir="${1:-}"
if [[ -n "$binary_dir" && -f "$binary_dir/CPackConfig.cmake" ]]; then
    grep -F 'set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE' "$binary_dir/CPackConfig.cmake" >/dev/null
    grep -F 'set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE' "$binary_dir/CPackConfig.cmake" >/dev/null
fi



