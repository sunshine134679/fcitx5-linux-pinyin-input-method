#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

# 独立调用这些回归也不使用日常目录或桌面会话；每个case仍可显式覆盖。
for variable in "${!MODERNIME_@}"; do unset "$variable"; done
# root容器也仅允许安装到本测试自建的隔离目录。
if [[ "$EUID" -eq 0 ]]; then export MODERNIME_ALLOW_ROOT=1; fi
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY SESSION_MANAGER DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE
unset XDG_DATA_HOME XDG_DESKTOP_DIR XDG_CURRENT_DESKTOP GNOME_DESKTOP_SESSION_ID DESKTOP_SESSION
export HOME="$test_root/home" XDG_CONFIG_HOME="$test_root/config"
export XDG_CACHE_HOME="$test_root/cache" XDG_STATE_HOME="$test_root/state"
export XDG_RUNTIME_DIR="$test_root/runtime" GSETTINGS_BACKEND=memory
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/nonexistent-bus"
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export FAKE_DESKTOP_LOG="$test_root/desktop-calls.log"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
: > "$FAKE_DESKTOP_LOG"

fake_bin="$test_root/bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$fake_bin/ctest" <<'EOF'
#!/usr/bin/env bash
printf 'ctest|DISPLAY=%s|WAYLAND=%s|GDK=%s|BROADWAY=%s\n' \
    "${DISPLAY-}" "${WAYLAND_DISPLAY-}" "${GDK_BACKEND-}" \
    "${BROADWAY_DISPLAY-}" >>"$FAKE_LOG"
exit 0
EOF
cat >"$fake_bin/pkg-config" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' /fake/lib
EOF
cat >"$fake_bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
printf 'fcitx5|DISPLAY=%s|DBUS=%s|RUNTIME=%s|ADDONS=%s|DATA=%s|GTEST=%s\n' \
    "${DISPLAY-}" "${DBUS_SESSION_BUS_ADDRESS-}" \
    "${XDG_RUNTIME_DIR-}" "${FCITX_ADDON_DIRS-}" "${XDG_DATA_DIRS-}" "${GTEST_OUTPUT-}" >>"$FAKE_LOG"
exit 0
EOF
cat >"$fake_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf 'remote:%s|DISPLAY=%s|DBUS=%s|RUNTIME=%s|ADDONS=%s|DATA=%s|GTEST=%s\n' \
    "${1-}" "${DISPLAY-}" "${DBUS_SESSION_BUS_ADDRESS-}" \
    "${XDG_RUNTIME_DIR-}" "${FCITX_ADDON_DIRS-}" "${XDG_DATA_DIRS-}" "${GTEST_OUTPUT-}" >>"$FAKE_LOG"
case "${1-}" in
    -n) printf '%s\n' modernime ;;
    '') printf '%s\n' 2 ;;
esac
exit 0
EOF
cat >"$fake_bin/pgrep" <<'EOF'
#!/usr/bin/env bash
if [[ ! -e "$FAKE_PGREP_STATE" ]]; then
    : >"$FAKE_PGREP_STATE"
    printf '%s\n' 100
else
    printf '%s\n' 101
fi
EOF

for desktop_tool in gio gnome-extensions gsettings glib-compile-schemas; do
    cat >"$fake_bin/$desktop_tool" <<'EOF'
#!/usr/bin/env bash
printf '%s|DBUS=%s|RUNTIME=%s\n' "${0##*/}" "${DBUS_SESSION_BUS_ADDRESS-}" \
    "${XDG_RUNTIME_DIR-}" >> "$FAKE_DESKTOP_LOG"
if [[ "${0##*/}" == gsettings && "${1-}" == get ]]; then printf '@as []\n'; fi
exit 0
EOF
done
chmod +x "$fake_bin"/*

log_file="$test_root/calls.log"
state_file="$test_root/pgrep.state"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    XDG_DATA_DIRS=/usr/local/share:/usr/share \
    HOME="$test_root/home" \
    XDG_CONFIG_HOME="$test_root/config" \
    MODERNIME_PREFIX="$test_root/prefix" \
    MODERNIME_BUILD_DIR="$test_root/build" \
    DISPLAY=:0 \
    GTEST_OUTPUT="xml:$test_root/caller-report.xml" \
    DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    FAKE_LOG="$log_file" \
    FAKE_PGREP_STATE="$state_file" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash ./install.sh >/dev/null

grep -F "|GTEST=xml:$test_root/caller-report.xml" "$log_file" >/dev/null

expected_addons="$test_root/prefix/lib/fcitx5:/fake/lib/fcitx5"
grep -F 'ctest|DISPLAY=|WAYLAND=|GDK=|BROADWAY=' "$log_file" >/dev/null
grep -F "DISPLAY=:0|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-s|DISPLAY=:0|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-o|DISPLAY=:0|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-n|DISPLAY=:0|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:|DISPLAY=:0|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR|ADDONS=$expected_addons" \
    "$log_file" >/dev/null

grep -F "|DATA=$test_root/prefix/share:/usr/local/share:/usr/share" "$log_file" >/dev/null
grep -F 'XDG_DATA_DIRS=' "$test_root/config/environment.d/90-modernime.conf" >/dev/null
grep -F 'XDG_DATA_DIRS=' "$test_root/config/autostart/modernime-fcitx5-session.desktop" >/dev/null

# Verify modernime-session-init works for independent user session initialization
session_config="$test_root/session-config"
export PATH="$fake_bin:/usr/bin:/bin"
env \
    MODERNIME_PREFIX="$test_root/prefix" \
    MODERNIME_CONFIG_HOME="$session_config" \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    bash "$project_root/scripts/modernime-session-init" >/dev/null

test -f "$session_config/environment.d/90-modernime.conf"
test -f "$session_config/autostart/modernime-fcitx5-session.desktop"
test -f "$session_config/fcitx5/profile"
grep -F 'Name=modernime' "$session_config/fcitx5/profile" >/dev/null

# 元数据/扩展调用由假桌面工具承接，但环境恢复仍按真实调用值验证。
test -s "$FAKE_DESKTOP_LOG"
while IFS= read -r desktop_call; do
    [[ "$desktop_call" == *"|DBUS=$DBUS_SESSION_BUS_ADDRESS|RUNTIME=$XDG_RUNTIME_DIR" ]]
done < "$FAKE_DESKTOP_LOG"
