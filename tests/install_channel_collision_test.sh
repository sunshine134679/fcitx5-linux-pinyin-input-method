#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-channel-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

fake_bin="$test_root/bin"
mkdir -p "$fake_bin"

cat >"$fake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
if [[ "${1-}" == "--version" ]]; then
    printf 'cmake version 3.28.3\n'
    exit 0
fi
prev=""
for arg in "$@"; do
    if [[ "$prev" == "-B" ]]; then
        mkdir -p "$arg"
        target_libdir="${FAKE_LIBDIR:-lib/x86_64-linux-gnu}"
        printf 'CMAKE_INSTALL_LIBDIR:PATH=%s\n' "$target_libdir" > "$arg/CMakeCache.txt"
    fi
    prev="$arg"
done
if [[ " $* " == *" --install "* ]]; then
    target_prefix="${FAKE_PREFIX:-/usr}"
    target_libdir="${FAKE_LIBDIR:-lib/x86_64-linux-gnu}"
    mkdir -p "$target_prefix/$target_libdir/fcitx5" \
             "$target_prefix/bin" \
             "$target_prefix/share/fcitx5/addon" \
             "$target_prefix/share/fcitx5/inputmethod" \
             "$target_prefix/share/applications" \
             "$target_prefix/share/modernime/pinyin"
    : >"$target_prefix/$target_libdir/fcitx5/modernime_fcitx5.so"
    : >"$target_prefix/$target_libdir/fcitx5/modernime_ui.so"
    : >"$target_prefix/bin/modernime-settings"
    : >"$target_prefix/bin/modernime-session-init"
    : >"$target_prefix/share/fcitx5/addon/modernime.conf"
    : >"$target_prefix/share/fcitx5/addon/modernime-ui.conf"
    : >"$target_prefix/share/fcitx5/inputmethod/modernime.conf"
    : >"$target_prefix/share/applications/modernime-settings.desktop"
    : >"$target_prefix/share/modernime/pinyin/modernime-knowledge.dict"
    : >"$target_prefix/share/modernime/pinyin/modernime-hotwords.dict"
    : >"$target_prefix/share/modernime/pinyin/modernime-english-dict.bin"
fi
exit 0
EOF

cat >"$fake_bin/ctest" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

cat >"$fake_bin/pkg-config" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' /fake/lib
EOF

cat >"$fake_bin/fcitx5" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

cat >"$fake_bin/fcitx5-remote" <<'EOF'
#!/usr/bin/env bash
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

cat >"$fake_bin/dpkg" <<'EOF'
#!/usr/bin/env bash
if [[ "${1-}" == "-S" ]]; then
    target_file="${2-}"
    if [[ -n "${FAKE_DPKG_OWNED_FILE-}" && "$target_file" == "$FAKE_DPKG_OWNED_FILE"* ]]; then
        printf 'modernime: %s\n' "$target_file"
        exit 0
    fi
fi
exit 1
EOF

chmod +x "$fake_bin"/*

# -------------------------------------------------------------
# Test 1: Migration of legacy v0.1.6 plugins from flat lib/fcitx5
# -------------------------------------------------------------
prefix_mig="$test_root/migration-prefix"
mkdir -p "$prefix_mig/lib/fcitx5"
echo "legacy_fcitx5_plugin" > "$prefix_mig/lib/fcitx5/modernime_fcitx5.so"
echo "legacy_ui_plugin" > "$prefix_mig/lib/fcitx5/modernime_ui.so"

env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/mig-home" \
    XDG_CONFIG_HOME="$test_root/mig-config" \
    MODERNIME_PREFIX="$prefix_mig" \
    MODERNIME_BUILD_DIR="$test_root/mig-build" \
    FAKE_PREFIX="$prefix_mig" \
    FAKE_LIBDIR="lib/x86_64-linux-gnu" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" >/dev/null

# The legacy plugin files in lib/fcitx5 should have been removed
test ! -f "$prefix_mig/lib/fcitx5/modernime_fcitx5.so"
test ! -f "$prefix_mig/lib/fcitx5/modernime_ui.so"
test ! -d "$prefix_mig/lib/fcitx5"

# The new multiarch plugin files should exist
test -f "$prefix_mig/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test -f "$prefix_mig/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"

# Manifest and channel tracking should exist
manifest="$prefix_mig/share/modernime/install-manifest.txt"
channel="$prefix_mig/share/modernime/install-channel.txt"
test -f "$manifest"
test -f "$channel"
grep -F 'channel=script' "$channel" >/dev/null
grep -F "$channel" "$manifest" >/dev/null

# Uninstall should cleanly remove the new files and prune empty multiarch directories
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/mig-home" \
    XDG_CONFIG_HOME="$test_root/mig-config" \
    MODERNIME_PREFIX="$prefix_mig" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/uninstall.sh" >/dev/null

test ! -f "$prefix_mig/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test ! -d "$prefix_mig/lib/x86_64-linux-gnu"
test ! -f "$manifest"
test ! -f "$channel"

# -------------------------------------------------------------
# Test 2: System prefix installation collision with native package
# -------------------------------------------------------------
sys_prefix="$test_root/sys-prefix"
mkdir -p "$sys_prefix/bin"
: >"$sys_prefix/bin/modernime-settings"
collision_output="$test_root/collision.log"

if env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/sys-home" \
    XDG_CONFIG_HOME="$test_root/sys-config" \
    MODERNIME_PREFIX="$sys_prefix" \
    MODERNIME_FORCE_SYSTEM_PREFIX=1 \
    FAKE_PREFIX="$sys_prefix" \
    FAKE_DPKG_OWNED_FILE="$sys_prefix/bin/modernime-settings" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" "$sys_prefix" >"$collision_output" 2>&1; then
    echo "Expected install.sh to fail on native package collision" >&2
    exit 1
fi
grep -F 'managed by native system package' "$collision_output" >/dev/null
grep -F 'conflicts with your package manager' "$collision_output" >/dev/null

# With --overwrite-package (or MODERNIME_OVERWRITE_PACKAGE=1), collision is bypassed
bypass_output="$test_root/bypass.log"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/sys-home" \
    XDG_CONFIG_HOME="$test_root/sys-config" \
    MODERNIME_PREFIX="$sys_prefix" \
    MODERNIME_FORCE_SYSTEM_PREFIX=1 \
    FAKE_PREFIX="$sys_prefix" \
    FAKE_DPKG_OWNED_FILE="$sys_prefix/bin/modernime-settings" \
    MODERNIME_OVERWRITE_PACKAGE=1 \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" "$sys_prefix" >"$bypass_output" 2>&1

test -f "$sys_prefix/share/modernime/install-manifest.txt"

# -------------------------------------------------------------
# Test 3: System prefix uninstall preserves native package files
# -------------------------------------------------------------
# Emulate uninstall where modernime-settings is reported as owned by dpkg
sys_preserve_prefix="$test_root/sys-preserve"
mkdir -p "$sys_preserve_prefix/bin" "$sys_preserve_prefix/share/modernime"
: >"$sys_preserve_prefix/bin/modernime-settings"
: >"$sys_preserve_prefix/share/modernime/install-channel.txt"
cat >"$sys_preserve_prefix/share/modernime/install-manifest.txt" <<EOF
$sys_preserve_prefix/bin/modernime-settings
$sys_preserve_prefix/share/modernime/install-channel.txt
EOF

env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/sys-home" \
    XDG_CONFIG_HOME="$test_root/sys-config" \
    MODERNIME_PREFIX="$sys_preserve_prefix" \
    MODERNIME_FORCE_SYSTEM_PREFIX=1 \
    FAKE_DPKG_OWNED_FILE="$sys_preserve_prefix/bin/modernime-settings" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/uninstall.sh" --prefix="$sys_preserve_prefix" >/dev/null

# File should be preserved because it is owned by native package
test -f "$sys_preserve_prefix/bin/modernime-settings"
# Channel file is not owned by package, so it should be removed
test ! -f "$sys_preserve_prefix/share/modernime/install-channel.txt"

printf 'All channel collision and legacy migration tests passed!\n'
