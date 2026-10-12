#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-upgrade-gate-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

prefix="$test_root/prefix"
config_home="$test_root/config"
data_home="$test_root/data"
desktop_dir="$test_root/Desktop"
home_dir="$test_root/home"

mkdir -p "$prefix/lib/fcitx5" \
         "$prefix/share/modernime/pinyin" \
         "$prefix/share/fcitx5/addon" \
         "$prefix/share/fcitx5/inputmethod" \
         "$prefix/share/applications" \
         "$prefix/bin" \
         "$config_home/modernime" \
         "$data_home/modernime" \
         "$desktop_dir" \
         "$home_dir"

printf '=== 1. 模拟 v0.1.6 旧版安装与用户持久化数据 ===\n'
# 1.1 模拟旧版扁平目录插件与二进制
printf 'v0.1.6-fcitx5-plugin\n' > "$prefix/lib/fcitx5/modernime_fcitx5.so"
printf 'v0.1.6-ui-plugin\n' > "$prefix/lib/fcitx5/modernime_ui.so"
printf '#!/bin/sh\necho "modernime-settings 0.1.6"\n' > "$prefix/bin/modernime-settings"
chmod +x "$prefix/bin/modernime-settings"
printf 'v0.1.6-knowledge\n' > "$prefix/share/modernime/pinyin/modernime-knowledge.dict"

# 1.2 模拟旧版 install-manifest.txt
cat > "$prefix/share/modernime/install-manifest.txt" <<EOF
$prefix/lib/fcitx5/modernime_fcitx5.so
$prefix/lib/fcitx5/modernime_ui.so
$prefix/share/modernime/pinyin/modernime-knowledge.dict
$prefix/bin/modernime-settings
EOF

# 1.3 模拟用户积累的个人词典、设置和学习数据库
printf '我的自定义短语\twodezidingyiduanju\t100\n' > "$data_home/modernime/user-dictionary.txt"
printf '[General]\nCandidateSize=9\n' > "$config_home/modernime/settings.conf"
printf 'SQLite format 3\x00user_learning_data_simulation' > "$data_home/modernime/learning.sqlite3"

user_dict_hash=$(sha256sum "$data_home/modernime/user-dictionary.txt" | awk '{print $1}')
settings_hash=$(sha256sum "$config_home/modernime/settings.conf" | awk '{print $1}')

printf '=== 2. 执行 v0.1.7 升级安装 ===\n'
export HOME="$home_dir"
export MODERNIME_PREFIX="$prefix"
export MODERNIME_CONFIG_HOME="$config_home"
export MODERNIME_DATA_HOME="$data_home"
export MODERNIME_DESKTOP_DIR="$desktop_dir"
export MODERNIME_SKIP_FCITX_RESTART=1
export MODERNIME_SKIP_TESTS=1
export XDG_CONFIG_HOME="$config_home"
export XDG_DATA_HOME="$data_home"

(
    cd "$project_root"
    bash ./install.sh "$prefix"
)

printf '=== 3. 验证升级完整性与旧版插件隔离 ===\n'
# 3.1 旧版扁平插件必须已被迁移清理，避免多架构符号碰撞
test ! -f "$prefix/lib/fcitx5/modernime_fcitx5.so"
test ! -f "$prefix/lib/fcitx5/modernime_ui.so"
test ! -d "$prefix/lib/fcitx5"

# 3.2 新版组件必须完整安装
test -f "$prefix/bin/modernime-settings"
test -f "$prefix/bin/modernime-session-init"
test -x "$prefix/bin/modernime-session-init"
multiarch_addon=$(find "$prefix" -type f -name 'modernime_fcitx5.so' -print -quit)
test -n "$multiarch_addon"
test -f "$multiarch_addon"

# 3.3 验证用户数据完整保留且哈希不变
test -f "$data_home/modernime/user-dictionary.txt"
test -f "$config_home/modernime/settings.conf"
current_dict_hash=$(sha256sum "$data_home/modernime/user-dictionary.txt" | awk '{print $1}')
current_settings_hash=$(sha256sum "$config_home/modernime/settings.conf" | awk '{print $1}')
test "$user_dict_hash" = "$current_dict_hash"
test "$settings_hash" = "$current_settings_hash"

# 3.4 验证学习数据库已自动创建备份
backup_db=$(find "$data_home/modernime" -name 'learning.sqlite3.backup-*' -print -quit)
test -n "$backup_db"
test -f "$backup_db"

printf '=== 4. 验证普通用户会话初始化脚本 (modernime-session-init) ===\n'
env \
    MODERNIME_PREFIX="$prefix" \
    MODERNIME_CONFIG_HOME="$config_home" \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    bash "$prefix/bin/modernime-session-init" >/dev/null

test -f "$config_home/environment.d/90-modernime.conf"
test -f "$config_home/autostart/modernime-fcitx5-session.desktop"
test -f "$config_home/fcitx5/profile"
grep -F 'Name=modernime' "$config_home/fcitx5/profile" >/dev/null

printf '=== 5. 执行对称卸载 ===\n'
(
    cd "$project_root"
    bash ./uninstall.sh --prefix="$prefix"
)

printf '=== 6. 验证卸载彻底性与用户数据安全 ===\n'
# 软件程序与配置必须彻底移除
test ! -e "$prefix/bin/modernime-settings"
test ! -e "$prefix/bin/modernime-session-init"
test ! -e "$prefix/share/modernime"
test ! -e "$prefix/share/fcitx5/addon/modernime.conf"
test ! -e "$desktop_dir/modernime-settings.desktop"
test ! -e "$config_home/autostart/modernime-fcitx5-session.desktop"
test ! -e "$config_home/environment.d/90-modernime.conf"

# 用户词典和设置必须安全保留，不被误删
test -f "$data_home/modernime/user-dictionary.txt"
test -f "$config_home/modernime/settings.conf"

printf '=== 7. 发布与升级全链路验证 100%% 通过！ ===\n'
