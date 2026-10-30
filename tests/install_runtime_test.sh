#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-install-runtime-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT
unset XDG_DATA_HOME MODERNIME_DATA_HOME MODERNIME_SKIP_FCITX_RESTART

make_fake_build_tools() {
    local fake_bin=$1
    mkdir -p "$fake_bin"

    cat >"$fake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
if [[ "${1-}" == "--version" ]]; then
    printf 'cmake version 3.28.3\n'
    exit 0
fi
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
for command_name in awk bash cat chmod cmp cp dirname find grep mkdir mktemp mv rm rmdir sed sort wc flock stat; do
    ln -s "$(command -v "$command_name")" "$no_fcitx_bin/$command_name"
done
ln -s "$(command -v env)" "$no_fcitx_bin/env"

no_fcitx_output="$test_root/no-fcitx-output"
if env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/no-fcitx-home" \
    XDG_CONFIG_HOME="$test_root/no-fcitx-config" \
    MODERNIME_PREFIX="$test_root/no-fcitx-prefix" \
    MODERNIME_BUILD_DIR="$test_root/no-fcitx-build" \
    PATH="$no_fcitx_bin" \
    bash "$project_root/install.sh" >"$no_fcitx_output" 2>&1; then
    echo "Expected install.sh to fail without fcitx5" >&2
    exit 1
fi
grep -F 'Missing required runtime dependencies' "$no_fcitx_output" >/dev/null
test ! -e "$test_root/no-fcitx-prefix/share/modernime/install-manifest.txt"

no_fcitx_output_allowed="$test_root/no-fcitx-output-allowed"
env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/no-fcitx-home" \
    XDG_CONFIG_HOME="$test_root/no-fcitx-config" \
    MODERNIME_PREFIX="$test_root/no-fcitx-prefix-allowed" \
    MODERNIME_BUILD_DIR="$test_root/no-fcitx-build-allowed" \
    PATH="$no_fcitx_bin" \
    bash "$project_root/install.sh" --allow-missing-deps >"$no_fcitx_output_allowed" 2>&1
grep -F 'file deployment completed (runtime activation skipped due to missing fcitx5)' \
    "$no_fcitx_output_allowed" >/dev/null
test -f "$test_root/no-fcitx-prefix-allowed/share/modernime/install-manifest.txt"

old_cmake_bin="$test_root/old-cmake-bin"
make_fake_build_tools "$old_cmake_bin"
cat >"$old_cmake_bin/cmake" <<'EOF'
#!/usr/bin/env bash
if [[ "${1-}" == "--version" ]]; then
    printf 'cmake version 3.22.1\n'
    exit 0
fi
exit 0
EOF
old_cmake_output="$test_root/old-cmake-output"
if env \
    -u MODERNIME_CONFIG_HOME \
    -u MODERNIME_DESKTOP_DIR \
    HOME="$test_root/old-cmake-home" \
    XDG_CONFIG_HOME="$test_root/old-cmake-config" \
    MODERNIME_PREFIX="$test_root/old-cmake-prefix" \
    MODERNIME_BUILD_DIR="$test_root/old-cmake-build" \
    PATH="$old_cmake_bin:$fake_bin:/usr/bin:/bin" \
    bash "$project_root/install.sh" >"$old_cmake_output" 2>&1; then
    echo "Expected install.sh to fail with CMake 3.22.1" >&2
    exit 1
fi
grep -F 'CMake 3.24 or higher is required' "$old_cmake_output" >/dev/null


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

# Verify automatic consistent online WAL backup of user learning database during upgrade
backup_data="$test_root/user-data"
mkdir -p "$backup_data/modernime"
"$project_root/scripts/modernime-data-guard" --init-lmdb "$backup_data/modernime/learning.lmdb"
python3 - "$backup_data/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
db_path = sys.argv[1]
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("CREATE TABLE learning_entries (id INTEGER PRIMARY KEY, pinyin TEXT, phrase TEXT);")
conn.execute("INSERT INTO learning_entries VALUES (1, 'ceshi', '测试');")
conn.commit()
conn.close()
PYEOF
backup_log="$test_root/backup.log"
run_install backup \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    MODERNIME_DATA_HOME="$backup_data" >"$backup_log" 2>&1
grep -F 'Backed up user learning database to' "$backup_log" >/dev/null
grep -F 'Backed up user LMDB learning database to' "$backup_log" >/dev/null
backup_file=$(find "$backup_data/modernime" -name 'learning.sqlite3.backup-*' -print -quit)
test -n "$backup_file"
test -f "$backup_file"
lmdb_backup_file=$(find "$backup_data/modernime" -name 'learning.lmdb.backup-*' -print -quit)
test -n "$lmdb_backup_file"
test -f "$lmdb_backup_file"
"$project_root/scripts/modernime-data-guard" --verify --data-dir "$backup_data/modernime"
python3 - "$backup_file" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("PRAGMA integrity_check;").fetchone()[0] == "ok"
assert conn.execute("SELECT phrase FROM learning_entries WHERE pinyin='ceshi';").fetchone()[0] == "测试"
conn.close()
PYEOF

# Verify corrupt SQLite and corrupt LMDB databases reject silent cp fallback
corrupt_data="$test_root/corrupt-user-data"
mkdir -p "$corrupt_data/modernime"
printf 'corrupt-non-sqlite-payload\n' > "$corrupt_data/modernime/learning.sqlite3"
if run_install corrupt_backup \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    MODERNIME_DATA_HOME="$corrupt_data" >/dev/null 2>&1; then
    echo "Expected install.sh to fail when learning.sqlite3 is corrupt" >&2
    exit 1
fi
test -z "$(find "$corrupt_data/modernime" -name 'learning.sqlite3.backup-*' -print -quit)"

corrupt_lmdb_data="$test_root/corrupt-lmdb-data"
mkdir -p "$corrupt_lmdb_data/modernime"
printf 'corrupt-non-lmdb-payload\n' > "$corrupt_lmdb_data/modernime/learning.lmdb"
if run_install corrupt_lmdb_backup \
    MODERNIME_SKIP_FCITX_RESTART=1 \
    MODERNIME_DATA_HOME="$corrupt_lmdb_data" >/dev/null 2>&1; then
    echo "Expected install.sh to fail when learning.lmdb is corrupt (must not fall back to cp)" >&2
    exit 1
fi
test -z "$(find "$corrupt_lmdb_data/modernime" -name 'learning.lmdb.backup-*' -print -quit)"

# Verify staged payload rollback on failure
stage_test_root="$test_root/stage-test"
mkdir -p "$stage_test_root/payload/usr/bin" \
         "$stage_test_root/payload/usr/share/modernime/pinyin" \
         "$stage_test_root/payload/usr/lib/x86_64-linux-gnu/fcitx5" \
         "$stage_test_root/dest/bin" \
         "$stage_test_root/dest/share/modernime/pinyin"

echo "original-v1" > "$stage_test_root/dest/bin/modernime-settings"
echo "original-dict" > "$stage_test_root/dest/share/modernime/pinyin/modernime-knowledge.dict"
echo "new-v2" > "$stage_test_root/payload/usr/bin/modernime-settings"
echo "new-dict" > "$stage_test_root/payload/usr/share/modernime/pinyin/modernime-knowledge.dict"
: > "$stage_test_root/payload/usr/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
chmod +x "$stage_test_root/payload/usr/bin/modernime-settings"

# Verify generated binary installer strips check_cmake_version so Ubuntu 22.04 (CMake 3.22) works
bash "$project_root/scripts/generate_binary_installer.sh" "$stage_test_root/payload"
! grep -F 'check_cmake_version' "$stage_test_root/payload/install.sh" >/dev/null

# Test successful staged install
project_root_main="$project_root"
(
    prefix="$stage_test_root/dest"
    project_root="$stage_test_root/payload"
    source "$project_root_main/scripts/install_binary_payload.sh"
)
grep -Fqx 'new-v2' "$stage_test_root/dest/bin/modernime-settings"

# Test rollback on staged payload failure (missing required binary)
rm -f "$stage_test_root/payload/usr/bin/modernime-settings"
if (
    prefix="$stage_test_root/dest"
    project_root="$stage_test_root/payload"
    source "$project_root_main/scripts/install_binary_payload.sh"
) 2>/dev/null; then
    echo "Expected staged install to fail when payload is broken" >&2
    exit 1
fi
# Previous version in dest must still be intact
grep -Fqx 'new-v2' "$stage_test_root/dest/bin/modernime-settings"

# Refusal from a temporary directory must happen before user data backup;
# unrelated CLI flags do not authorize writing to the default user prefix.
guard_package="$test_root/guard-package"
mkdir -p "$guard_package/scripts" "$test_root/guard-data/modernime"
cp "$project_root/install.sh" "$guard_package/install.sh"
cp "$project_root/scripts/install_state.sh" "$guard_package/scripts/install_state.sh"
cp "$project_root/scripts/install_lifecycle.sh" "$guard_package/scripts/install_lifecycle.sh"
cat > "$guard_package/scripts/modernime-data-guard" <<'GUARD'
import os
with open(os.environ["TEST_GUARD_LOG"], "a") as log:
    log.write("backup was called\n")
GUARD
cp "$guard_package/scripts/modernime-data-guard" "$stage_test_root/payload/scripts/modernime-data-guard"
for guard_installer in "$guard_package/install.sh" "$stage_test_root/payload/install.sh"; do
for mode in no-prefix flags-only; do
    guard_args=()
    [[ "$mode" == flags-only ]] && guard_args+=(--allow-missing-deps)
    if env -u MODERNIME_PREFIX -u SUDO_USER \
        HOME="$test_root/guard-home" MODERNIME_BUILD_DIR="$test_root/guard-build" \
        MODERNIME_CONFIG_HOME="$test_root/guard-config" MODERNIME_DATA_HOME="$test_root/guard-data" \
        MODERNIME_DESKTOP_DIR="$test_root/guard-desktop" MODERNIME_SKIP_FCITX_RESTART=1 \
        TEST_GUARD_LOG="$test_root/guard-calls-$mode" PATH="$fake_bin:/usr/bin:/bin" \
        bash "$guard_installer" "${guard_args[@]}" > "$test_root/guard-output-$mode" 2>&1; then
        printf 'Expected temporary-directory refusal for %s\n' "$mode" >&2
        exit 1
    fi
    grep -F 'Running install.sh from temporary directory' "$test_root/guard-output-$mode" >/dev/null
    if [[ -s "$test_root/guard-calls-$mode" ]]; then
        printf 'Unsafe temporary install touched user data before refusal (%s)\n' "$mode" >&2
        exit 1
    fi
    test ! -e "$test_root/guard-home/.local"
done
done
printf 'Source/binary install runtime and early temporary-directory protection passed\n'
