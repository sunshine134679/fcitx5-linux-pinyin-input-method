#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

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
printf 'fcitx5|DISPLAY=%s|DBUS=%s|RUNTIME=%s|ADDONS=%s\n' \
    "${DISPLAY-}" "${DBUS_SESSION_BUS_ADDRESS-}" \
    "${XDG_RUNTIME_DIR-}" "${FCITX_ADDON_DIRS-}" >>"$FAKE_LOG"
exit 0
EOF
cat >"$fake_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf 'remote:%s|DISPLAY=%s|DBUS=%s|RUNTIME=%s|ADDONS=%s\n' \
    "${1-}" "${DISPLAY-}" "${DBUS_SESSION_BUS_ADDRESS-}" \
    "${XDG_RUNTIME_DIR-}" "${FCITX_ADDON_DIRS-}" >>"$FAKE_LOG"
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
chmod +x "$fake_bin"/*

log_file="$test_root/calls.log"
state_file="$test_root/pgrep.state"
env \
    HOME="$test_root/home" \
    XDG_CONFIG_HOME="$test_root/config" \
    MODERNIME_PREFIX="$test_root/prefix" \
    MODERNIME_BUILD_DIR="$test_root/build" \
    DISPLAY=:0 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    XDG_RUNTIME_DIR=/run/user/1000 \
    FAKE_LOG="$log_file" \
    FAKE_PGREP_STATE="$state_file" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash ./install.sh >/dev/null

expected_addons="$test_root/prefix/lib/fcitx5:/fake/lib/fcitx5"
grep -F 'ctest|DISPLAY=|WAYLAND=|GDK=|BROADWAY=' "$log_file" >/dev/null
grep -F "DISPLAY=:0|DBUS=unix:path=/run/user/1000/bus|RUNTIME=/run/user/1000|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-s|DISPLAY=:0|DBUS=unix:path=/run/user/1000/bus|RUNTIME=/run/user/1000|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-o|DISPLAY=:0|DBUS=unix:path=/run/user/1000/bus|RUNTIME=/run/user/1000|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:-n|DISPLAY=:0|DBUS=unix:path=/run/user/1000/bus|RUNTIME=/run/user/1000|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
grep -F "remote:|DISPLAY=:0|DBUS=unix:path=/run/user/1000/bus|RUNTIME=/run/user/1000|ADDONS=$expected_addons" \
    "$log_file" >/dev/null
