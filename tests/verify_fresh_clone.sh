#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-fresh-clone-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

printf '=== 1. 克隆代码仓库到沙箱隔离目录 ===\n'
# Build a local snapshot first: cloning HEAD alone silently validates the old
# version when this script is run before committing a fix. The original index,
# branches and user files remain untouched.
git clone --no-hardlinks "$project_root" "$test_root/snapshot"
git -C "$project_root" diff --binary HEAD > "$test_root/working-tree.patch"
if [[ -s "$test_root/working-tree.patch" ]]; then
    git -C "$test_root/snapshot" apply "$test_root/working-tree.patch"
fi
while IFS= read -r -d '' path; do
    mkdir -p -- "$test_root/snapshot/$(dirname -- "$path")"
    cp -a -- "$project_root/$path" "$test_root/snapshot/$path"
done < <(git -C "$project_root" ls-files --others --exclude-standard -z)
git -C "$test_root/snapshot" add -A
if ! git -C "$test_root/snapshot" diff --cached --quiet; then
    git -C "$test_root/snapshot" -c user.name='ModernIME Verification' \
        -c user.email='verification@localhost' -c commit.gpgsign=false \
        -c core.hooksPath=/dev/null commit -m 'Isolated verification snapshot' >/dev/null
fi
git clone --no-hardlinks "$test_root/snapshot" "$test_root/repo"

mkdir -p "$test_root/home"
export HOME="$test_root/home"
export XDG_DATA_HOME="$test_root/data"
export XDG_CONFIG_HOME="$test_root/config"
export XDG_CACHE_HOME="$test_root/cache"
export MODERNIME_PREFIX="$test_root/prefix"
export MODERNIME_CONFIG_HOME="$test_root/config"
export MODERNIME_DESKTOP_DIR="$test_root/Desktop"
export MODERNIME_SKIP_FCITX_RESTART=1

printf '=== 2. 在沙箱环境中执行 install.sh ===\n'
(cd "$test_root/repo" && ./install.sh)

printf '=== 3. 验证沙箱安装产物完整性 ===\n'
test -f "$MODERNIME_PREFIX/bin/modernime-settings"
test -x "$MODERNIME_PREFIX/bin/modernime-settings"
installed_version=$(env -u DISPLAY -u WAYLAND_DISPLAY "$MODERNIME_PREFIX/bin/modernime-settings" --version)
[[ "$installed_version" == *"ModernIME settings"* ]]
# Verify internal capture collector handles missing args by exiting non-zero safely
if env -u DISPLAY -u WAYLAND_DISPLAY "$MODERNIME_PREFIX/bin/modernime-settings" --internal-capture-collector 2>/dev/null; then
    echo "ERROR: Expected non-zero exit when internal capture collector is invoked without args" >&2
    exit 1
fi

# Execute a synthetic task with installed binary in headless environment and verify full result
t4_opdir=$(mktemp -d "$test_root/modernime-install-collector-XXXXXX")
chmod 0700 "$t4_opdir"
t4_spec="$t4_opdir/spec.dat"
cat << 'EOF' > "$t4_spec"
operationId=98765
token=install-token-98765
timeoutMs=5000
mode=1
perStreamBufferBytes=1048576
perStreamResultBytes=65536
perStreamFileBytes=1048576
argc=3
argv=/usr/bin/python3
argv=-c
argv=import sys; sys.stdout.write("INSTALL_COLLECTOR_STDOUT_OK\n"); sys.stderr.write("INSTALL_COLLECTOR_STDERR_OK\n")
envc=0
EOF
chmod 0600 "$t4_spec"

t4_pipe="$t4_opdir/handshake.fifo"
mkfifo "$t4_pipe"
exec 3<>"$t4_pipe"
env -u DISPLAY -u WAYLAND_DISPLAY "$MODERNIME_PREFIX/bin/modernime-settings" \
    --internal-capture-collector \
    --op-dir "$t4_opdir" \
    --handshake-fd 3 &
t4_pid=$!

if ! read -r -n 1 -t 5 t4_hs <&3; then
    kill -9 "$t4_pid" 2>/dev/null || true
    echo "ERROR: Handshake read timed out" >&2
    exit 1
fi
exec 3>&-
exec 3<&-

if [[ "$t4_hs" != "R" && "$t4_hs" != "1" ]]; then
    kill -9 "$t4_pid" 2>/dev/null || true
    echo "ERROR: Unexpected handshake byte: $t4_hs" >&2
    exit 1
fi

wait "$t4_pid"
t4_status=$?
if [[ "$t4_status" -ne 0 ]]; then
    echo "ERROR: Collector process exited with non-zero: $t4_status" >&2
    exit 1
fi

test -f "$t4_opdir/state.json"
test -f "$t4_opdir/stdout.tail"
test -f "$t4_opdir/stderr.tail"
grep -q '"state": "Exited"' "$t4_opdir/state.json"
grep -q '"exitStatus": 0' "$t4_opdir/state.json"
grep -q '"operationId": 98765' "$t4_opdir/state.json"
grep -q '"token": "install-token-98765"' "$t4_opdir/state.json"
grep -q "INSTALL_COLLECTOR_STDOUT_OK" "$t4_opdir/stdout.tail"
grep -q "INSTALL_COLLECTOR_STDERR_OK" "$t4_opdir/stderr.tail"
rm -rf -- "$t4_opdir"
test -f "$MODERNIME_PREFIX/bin/modernime-session-init"
test -f "$MODERNIME_PREFIX/share/modernime/install_state.sh"
test -x "$MODERNIME_PREFIX/bin/modernime-session-init"
{ test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so" || \
  test -f "$MODERNIME_PREFIX/lib64/fcitx5/modernime_fcitx5.so" || \
  test -f "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"; }
{ test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so" || \
  test -f "$MODERNIME_PREFIX/lib64/fcitx5/modernime_ui.so" || \
  test -f "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"; }
test -f "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
test -f "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test -f "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
grep -Fx 'Name=modernime' "$MODERNIME_CONFIG_HOME/fcitx5/profile" >/dev/null
test -x "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
for icon_size in 16 32 48 64 128 256 512; do
    test -s "$MODERNIME_PREFIX/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
done
grep -Fx "Icon=$MODERNIME_PREFIX/share/icons/hicolor/512x512/apps/modernime.png" "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop" >/dev/null

# Validate actual Fcitx metadata discovery using the installed session environment.
set -a
source "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
set +a
"$test_root/repo/build/install-debug/tests/modernime_install_discovery_tests" "$MODERNIME_PREFIX"
test ! -e "$HOME/.xprofile"

printf '=== 4. 在沙箱环境中执行 uninstall.sh ===\n'
(cd "$test_root/repo" && ./uninstall.sh)

printf '=== 5. 验证沙箱卸载清理完全性 ===\n'
test ! -e "$MODERNIME_PREFIX/bin/modernime-settings"
test ! -e "$MODERNIME_PREFIX/bin/modernime-session-init"
test ! -e "$MODERNIME_PREFIX/share/modernime/install_state.sh"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/LICENSE"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/copyright"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/THIRD_PARTY_NOTICES.md"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/SOURCES.json"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib64/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_PREFIX/lib64/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
for icon_size in 16 32 48 64 128 256 512; do
    test ! -e "$MODERNIME_PREFIX/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
done
test ! -e "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test ! -e "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"

printf '=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100%% 通过！ ===\n'
