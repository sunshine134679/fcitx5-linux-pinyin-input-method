#!/usr/bin/env bash
set -euo pipefail
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-session-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/bin" "$test_root/prefix/lib/fcitx5" "$test_root/home"
: > "$test_root/prefix/lib/fcitx5/modernime_fcitx5.so"
cat > "$test_root/bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
printf 'start|%s|%s|%s|%s\n' "$*" "${FCITX_ADDON_DIRS-}" "${XDG_DATA_DIRS-}" "${XDG_CONFIG_HOME-}" >> "$FAKE_LOG"
: > "$FAKE_RUNNING"
EOF
cat > "$test_root/bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
printf 'remote:%s|%s\n' "$*" "${XDG_CONFIG_HOME-}" >> "$FAKE_LOG"
if [[ "${FAKE_HANG:-0}" == 1 ]]; then exec sleep 10; fi
[[ -f "$FAKE_RUNNING" ]] || exit 1
case "${1-}" in
    '') printf '%s\n' "${FAKE_STATUS:-2}" ;;
    -n) [[ "${FAKE_STATUS:-2}" == 0 ]] || printf 'modernime\n' ;;
    -m) printf 'modernime\n' ;;
esac
EOF
chmod +x "$test_root/bin/"*
export HOME="$test_root/home" MODERNIME_PREFIX="$test_root/prefix"
export MODERNIME_CONFIG_HOME="$test_root/config" XDG_CONFIG_HOME="$test_root/wrong-config"
export PATH="$test_root/bin:/usr/bin:/bin" DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS=unix:path=/fake/bus
export XDG_DATA_DIRS=/usr/local/share:/usr/share FAKE_LOG="$test_root/calls" FAKE_RUNNING="$test_root/running"
unset MODERNIME_SKIP_FCITX_RESTART
bash "$project_root/scripts/modernime-session-init" > "$test_root/result"
if [[ ! -f "$FAKE_RUNNING" ]]; then
    printf 'session init failed to start a stopped Fcitx5\n' >&2; exit 1
fi
grep -F "|$MODERNIME_PREFIX/lib/fcitx5:" "$FAKE_LOG" >/dev/null
grep -F "|$MODERNIME_PREFIX/share:/usr/local/share:/usr/share|$MODERNIME_CONFIG_HOME" "$FAKE_LOG" >/dev/null
grep -F 'successfully' "$test_root/result" >/dev/null
grep -Fqx 'Name=modernime' "$MODERNIME_CONFIG_HOME/fcitx5/profile"
: > "$FAKE_LOG"
bash "$project_root/scripts/modernime-session-init" >/dev/null
if grep -q '^start|' "$FAKE_LOG"; then
    printf 'session init duplicated an already running Fcitx5\n' >&2; exit 1
fi
test "$(grep -c '^Name=modernime$' "$MODERNIME_CONFIG_HOME/fcitx5/profile")" = 1

# No focused editor is a healthy daemon state, not a failed activation.
FAKE_STATUS=0 bash "$project_root/scripts/modernime-session-init" > "$test_root/no-context"
grep -F 'successfully' "$test_root/no-context" >/dev/null

# Unowned files and symlinks must survive independent initialization.
export MODERNIME_CONFIG_HOME="$test_root/custom-config"
mkdir -p "$MODERNIME_CONFIG_HOME/environment.d" "$MODERNIME_CONFIG_HOME/autostart"
printf 'user environment\n' > "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
printf 'user autostart\n' > "$test_root/custom.desktop"
ln -s "$test_root/custom.desktop" "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
MODERNIME_SKIP_FCITX_RESTART=1 bash "$project_root/scripts/modernime-session-init" >/dev/null
grep -Fx 'user environment' "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
test -L "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
grep -Fx 'user autostart' "$test_root/custom.desktop"

# A wedged remote command cannot hang initialization indefinitely.
export MODERNIME_CONFIG_HOME="$test_root/hanging-config"
if ! timeout --kill-after=1s 8s env FAKE_HANG=1 bash "$project_root/scripts/modernime-session-init" > "$test_root/hanging"; then
    printf 'session initialization did not bound remote queries\n' >&2; exit 1
fi
grep -F 'could not' "$test_root/hanging" >/dev/null

# A custom prefix containing spaces must work both when sourcing session
# environment and when a desktop launcher parses the actual autostart entry.
export MODERNIME_PREFIX="$test_root/prefix with space"
export MODERNIME_CONFIG_HOME="$test_root/config with space"
mkdir -p "$MODERNIME_PREFIX/lib/fcitx5"
: > "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so"
MODERNIME_SKIP_FCITX_RESTART=1 bash "$project_root/scripts/modernime-session-init" >/dev/null
(
    set -a
    source "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
    set +a
    [[ "$FCITX_ADDON_DIRS" == "$MODERNIME_PREFIX/lib/fcitx5:"* ]]
    [[ "$XDG_DATA_DIRS" == "$MODERNIME_PREFIX/share:"* ]]
)
if command -v gio >/dev/null 2>&1; then
    : > "$FAKE_LOG"
    rm -f "$FAKE_RUNNING"
    gio launch "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
    for attempt in {1..30}; do
        [[ -f "$FAKE_RUNNING" ]] && break
        sleep 0.1
    done
    test -f "$FAKE_RUNNING"
    grep -F "|$MODERNIME_PREFIX/share:/usr/local/share:/usr/share|$MODERNIME_CONFIG_HOME" "$FAKE_LOG" >/dev/null
fi
