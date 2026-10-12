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

# -------------------------------------------------------------
# Test 4: Block overwriting unknown origin or mismatched channel (e.g. channel=deb)
# -------------------------------------------------------------
unknown_prefix="$test_root/unknown-prefix"
mkdir -p "$unknown_prefix/bin"
: > "$unknown_prefix/bin/modernime-settings"
unknown_log="$test_root/unknown.log"
if env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/sys-home" \
    XDG_CONFIG_HOME="$test_root/sys-config" \
    MODERNIME_PREFIX="$unknown_prefix" \
    FAKE_PREFIX="$unknown_prefix" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" "$unknown_prefix" >"$unknown_log" 2>&1; then
    echo "Expected install.sh to block overwriting unknown origin installation" >&2
    exit 1
fi
grep -F 'unknown origin' "$unknown_log" >/dev/null
grep -F 'modernime-migrate-channel --check' "$unknown_log" >/dev/null

deb_channel_prefix="$test_root/deb-channel-prefix"
mkdir -p "$deb_channel_prefix/share/modernime"
printf 'channel=deb\nversion=0.1.7\nprefix=%s\n' "$deb_channel_prefix" > "$deb_channel_prefix/share/modernime/install-channel.txt"
mismatched_log="$test_root/mismatched.log"
if env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/sys-home" \
    XDG_CONFIG_HOME="$test_root/sys-config" \
    MODERNIME_PREFIX="$deb_channel_prefix" \
    FAKE_PREFIX="$deb_channel_prefix" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" "$deb_channel_prefix" >"$mismatched_log" 2>&1; then
    echo "Expected install.sh to block overwriting channel=deb installation" >&2
    exit 1
fi
grep -F 'belongs to channel "deb"' "$mismatched_log" >/dev/null

# -------------------------------------------------------------
# Test 5, 6, 7: Controlled migration via modernime-migrate-channel (--check, rollback on failure, and successful migration)
# -------------------------------------------------------------
chmod +x "$project_root/scripts/modernime-migrate-channel"
mig_src_prefix="$test_root/mig-src"
mig_target_prefix="$test_root/mig-target-usr"
mig_data_home="$test_root/mig-data"
mig_cfg_home="$test_root/mig-cfg"
mkdir -p "$mig_data_home/modernime" "$mig_cfg_home/modernime"
printf 'input.toggle_key=Ctrl+Space\n' > "$mig_cfg_home/modernime/settings.conf"
printf 'qian\x27yi\t迁移词条\t88\n' > "$mig_data_home/modernime/user-dictionary.txt"
"$project_root/scripts/modernime-data-guard" --init-lmdb "$mig_data_home/modernime/learning.lmdb"
python3 - "$mig_data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("CREATE TABLE learning_entries (pinyin TEXT, phrase TEXT, context_before TEXT, context_after TEXT, frequency INTEGER, last_selected_ms INTEGER, negative_feedback INTEGER, suppressed INTEGER, PRIMARY KEY (pinyin, phrase, context_before, context_after));")
conn.execute("INSERT INTO learning_entries VALUES ('qian''yi', '迁移测试', '', '', 9, 1700000000, 0, 0);")
conn.commit()
conn.close()
PYEOF

env \
    HOME="$test_root/mig-user-home" \
    MODERNIME_CONFIG_HOME="$mig_cfg_home" \
    MODERNIME_DATA_HOME="$mig_data_home" \
    MODERNIME_DESKTOP_DIR="$test_root/mig-desktop" \
    MODERNIME_PREFIX="$mig_src_prefix" \
    MODERNIME_BUILD_DIR="$test_root/mig-src-build" \
    FAKE_PREFIX="$mig_src_prefix" \
    FAKE_LIBDIR="lib/x86_64-linux-gnu" \
    PATH="$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" "$mig_src_prefix" >/dev/null

# Test 5: --check output
check_out="$test_root/migrate-check.log"
env \
    MODERNIME_CONFIG_HOME="$mig_cfg_home" \
    MODERNIME_DATA_HOME="$mig_data_home" \
    "$project_root/scripts/modernime-migrate-channel" --check --from-prefix "$mig_src_prefix" >"$check_out"
grep -F 'Detected Channel:  script' "$check_out" >/dev/null
grep -F 'Detected Version:  0.1.8' "$check_out" >/dev/null

# Test 6: Migration failure rolls back to preserve original script installation and prints recovery instructions
fail_out="$test_root/migrate-fail.log"
if env \
    MODERNIME_CONFIG_HOME="$mig_cfg_home" \
    MODERNIME_DATA_HOME="$mig_data_home" \
    "$project_root/scripts/modernime-migrate-channel" \
        --from-prefix "$mig_src_prefix" \
        --target-prefix "$mig_target_prefix" \
        --to-channel deb \
        --package-cmd "false" \
        --no-session-init >"$fail_out" 2>&1; then
    echo "Expected modernime-migrate-channel to fail when package-cmd fails" >&2
    exit 1
fi
grep -F 'Original installation restored (channel=script' "$fail_out" >/dev/null
grep -F 'Recovery Instructions:' "$fail_out" >/dev/null
test -f "$mig_src_prefix/bin/modernime-settings"
test -f "$mig_src_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test -f "$mig_src_prefix/share/modernime/install-manifest.txt"
grep -F 'channel=script' "$mig_src_prefix/share/modernime/install-channel.txt" >/dev/null

# Test 7: Successful migration cleans up old script files, deploys native package, and verifies version/path
populate_deb_cmd="mkdir -p '$mig_target_prefix/bin' '$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5' '$mig_target_prefix/share/modernime/pinyin' && \
printf '#!/bin/sh\necho 0.1.8\n' > '$mig_target_prefix/bin/modernime-settings' && \
chmod +x '$mig_target_prefix/bin/modernime-settings' && \
: > '$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so' && \
: > '$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so' && \
: > '$mig_target_prefix/share/modernime/pinyin/modernime-knowledge.dict' && \
printf 'channel=deb\nversion=0.1.8\nprefix=%s\n' '$mig_target_prefix' > '$mig_target_prefix/share/modernime/install-channel.txt'"

# Test 6b: Post-install verification failure uninstalls newly installed target package and cleans target_prefix
broken_deb_cmd="mkdir -p '$mig_target_prefix/bin' '$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5' '$mig_target_prefix/share/modernime/pinyin' && \
printf '#!/bin/sh\necho 0.1.8\n' > '$mig_target_prefix/bin/modernime-settings' && \
chmod +x '$mig_target_prefix/bin/modernime-settings' && \
: > '$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so' && \
: > '$mig_target_prefix/share/modernime/pinyin/modernime-knowledge.dict' && \
printf 'channel=rpm\nversion=0.1.8\nprefix=%s\n' '$mig_target_prefix' > '$mig_target_prefix/share/modernime/install-channel.txt'"

pkg_rollback_marker="$test_root/pkg-rollback-ran.txt"
post_fail_out="$test_root/migrate-post-fail.log"
if env \
    MODERNIME_CONFIG_HOME="$mig_cfg_home" \
    MODERNIME_DATA_HOME="$mig_data_home" \
    "$project_root/scripts/modernime-migrate-channel" \
        --from-prefix "$mig_src_prefix" \
        --target-prefix "$mig_target_prefix" \
        --to-channel deb \
        --package-cmd "$broken_deb_cmd" \
        --package-rollback-cmd "printf 'uninstalled-deb\n' > '$pkg_rollback_marker'" \
        --no-session-init >"$post_fail_out" 2>&1; then
    echo "Expected modernime-migrate-channel to fail when post-install verification fails" >&2
    exit 1
fi
test -f "$pkg_rollback_marker"
grep -Fxq "uninstalled-deb" "$pkg_rollback_marker"
test ! -e "$mig_target_prefix/bin/modernime-settings"
test ! -e "$mig_target_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test -f "$mig_src_prefix/bin/modernime-settings"
grep -F 'channel=script' "$mig_src_prefix/share/modernime/install-channel.txt" >/dev/null

ok_out="$test_root/migrate-ok.log"
env \
    MODERNIME_CONFIG_HOME="$mig_cfg_home" \
    MODERNIME_DATA_HOME="$mig_data_home" \
    "$project_root/scripts/modernime-migrate-channel" \
        --from-prefix "$mig_src_prefix" \
        --target-prefix "$mig_target_prefix" \
        --to-channel deb \
        --package-cmd "$populate_deb_cmd" \
        --no-session-init >"$ok_out" 2>&1

grep -F 'Migration completed successfully!' "$ok_out" >/dev/null
grep -F 'New Channel: deb' "$ok_out" >/dev/null
grep -F "Plugin Path: $mig_target_prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so" "$ok_out" >/dev/null
test ! -f "$mig_src_prefix/bin/modernime-settings"
test ! -f "$mig_src_prefix/share/modernime/install-manifest.txt"
test -x "$mig_target_prefix/bin/modernime-settings"
grep -F '迁移词条' "$mig_data_home/modernime/user-dictionary.txt" >/dev/null
grep -F 'input.toggle_key=Ctrl+Space' "$mig_cfg_home/modernime/settings.conf" >/dev/null

printf 'All channel collision and legacy migration tests passed!\n'
