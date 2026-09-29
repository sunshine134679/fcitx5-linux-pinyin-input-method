#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-fresh-clone-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

printf '=== 1. 克隆代码仓库到沙箱隔离目录 ===\n'
git clone "$project_root" "$test_root/repo"

export MODERNIME_PREFIX="$test_root/prefix"
export MODERNIME_CONFIG_HOME="$test_root/config"
export MODERNIME_DESKTOP_DIR="$test_root/Desktop"
export MODERNIME_SKIP_FCITX_RESTART=1

printf '=== 2. 在沙箱环境中执行 install.sh ===\n'
(cd "$test_root/repo" && ./install.sh)

printf '=== 3. 验证沙箱安装产物完整性 ===\n'
test -f "$MODERNIME_PREFIX/bin/modernime-settings"
test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so"
test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so"
test -f "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
test -f "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test -f "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
test -x "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"

printf '=== 4. 在沙箱环境中执行 uninstall.sh ===\n'
(cd "$test_root/repo" && ./uninstall.sh)

printf '=== 5. 验证沙箱卸载清理完全性 ===\n'
test ! -e "$MODERNIME_PREFIX/bin/modernime-settings"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
test ! -e "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test ! -e "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"

printf '=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100%% 通过！ ===\n'
