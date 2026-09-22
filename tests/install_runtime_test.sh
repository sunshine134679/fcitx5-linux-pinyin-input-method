#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-runtime-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

make_fake_build_tools() {
    local fake_bin=$1
    mkdir -p "$fake_bin"

    cat >"$fake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
    cat >"$fake_bin/ctest" <<'EOF'
#!/usr/bin/env bash
if [[ -n "${FAKE_CTEST_SKIP_LOG:-}" ]]; then
    if [[ -n "${MODERNIME_SKIP_FCITX_RESTART:-}" ]]; then
        printf '%s\n' inherited >"$FAKE_CTEST_SKIP_LOG"
    else
        : >"$FAKE_CTEST_SKIP_LOG"
    fi
fi
exit 0
EOF
    cat >"$fake_bin/pkg-config" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' /fake/lib
EOF
    cat >"$fake_bin/xdg-user-dir" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
    chmod +x "$fake_bin"/*
}

fake_bin="$test_root/fake-bin"
make_fake_build_tools "$fake_bin"

log_file="$test_root/fcitx.log"
cat >"$fake_bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
printf 'fcitx5\n' >>"$FAKE_LOG"
exit 0
EOF
cat >"$fake_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf 'remote:%s\n' "${1-}" >>"$FAKE_LOG"
case "${1-}" in
    -n) printf '%s\n' modernime ;;
    '') printf '%s\n' 2 ;;
esac
exit 0
EOF
cat >"$fake_bin/pgrep" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' 100
EOF
chmod +x "$fake_bin/fcitx5" "$fake_bin/fcitx5-remote" "$fake_bin/pgrep"

run_install() {
    local name=$1
    shift
    env \
        -u MODERNIME_CONFIG_HOME \
        -u MODERNIME_DESKTOP_DIR \
        HOME="$test_root/$name-home" \
        XDG_CONFIG_HOME="$test_root/$name-config" \
        MODERNIME_PREFIX="$test_root/$name-prefix" \
        MODERNIME_BUILD_DIR="$test_root/$name-build" \
        FAKE_LOG="$log_file" \
        PATH="$fake_bin:/usr/bin:/bin" \
        "$@" \
        bash "$project_root/install.sh"
}

: >"$log_file"
ctest_skip_log="$test_root/ctest-skip.log"
skip_output="$test_root/skip-output"
run_install skip \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    FAKE_CTEST_SKIP_LOG="$ctest_skip_log" >"$skip_output" 2>&1
grep -F 'Fcitx5 restart skipped by MODERNIME_SKIP_FCITX_RESTART=1' \
    "$skip_output" >/dev/null
test ! -s "$log_file"
test ! -s "$ctest_skip_log"

no_fcitx_bin="$test_root/no-fcitx-bin"
make_fake_build_tools "$no_fcitx_bin"
for command_name in bash chmod cmp dirname grep mkdir mktemp mv rm sed wc; do
    ln -s "$(command -v "$command_name")" "$no_fcitx_bin/$command_name"
done
ln -s "$(command -v env)" "$no_fcitx_bin/env"

no_fcitx_output="$test_root/no-fcitx-output"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/no-fcitx-home" \
    XDG_CONFIG_HOME="$test_root/no-fcitx-config" \
    MODERNIME_PREFIX="$test_root/no-fcitx-prefix" \
    MODERNIME_BUILD_DIR="$test_root/no-fcitx-build" \
    PATH="$no_fcitx_bin" \
    bash "$project_root/install.sh" >"$no_fcitx_output" 2>&1
grep -F 'Fcitx5 executable not found; installation completed' \
    "$no_fcitx_output" >/dev/null
test -f "$test_root/no-fcitx-prefix/share/modernime/install-manifest.txt"

crashing_bin="$test_root/crashing-bin"
make_fake_build_tools "$crashing_bin"
cat >"$crashing_bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
printf 'fcitx5\n' >>"$FAKE_CRASH_LOG"
kill -ABRT "$$"
EOF
cat >"$crashing_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf 'remote:%s\n' "${1-}" >>"$FAKE_CRASH_LOG"
kill -ABRT "$$"
EOF
cat >"$crashing_bin/pgrep" <<'EOF'
#!/usr/bin/env bash
if [[ ! -e "$FAKE_CRASH_PGREP_STATE" ]]; then
    : >"$FAKE_CRASH_PGREP_STATE"
    printf '%s\n' 100
else
    printf '%s\n' 101
fi
EOF
chmod +x "$crashing_bin/fcitx5" "$crashing_bin/fcitx5-remote" \
    "$crashing_bin/pgrep"

crash_log="$test_root/crash.log"
crash_output="$test_root/crash-output"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/crash-home" \
    XDG_CONFIG_HOME="$test_root/crash-config" \
    MODERNIME_PREFIX="$test_root/crash-prefix" \
    MODERNIME_BUILD_DIR="$test_root/crash-build" \
    DISPLAY=:0 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
    FAKE_CRASH_LOG="$crash_log" \
    FAKE_CRASH_PGREP_STATE="$test_root/crash-pgrep.state" \
    PATH="$crashing_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" >"$crash_output" 2>&1
grep -F 'remote:-s' "$crash_log" >/dev/null
! grep -F 'Aborted' "$crash_output" >/dev/null
