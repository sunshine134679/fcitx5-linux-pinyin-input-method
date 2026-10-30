#!/usr/bin/env bash
# ModernIME Install Lifecycle Preparation & Rollback Fixture Test
# Exercises explicit thin entry points and validates the core invariant:
# RollbackPreservesConcurrentUserWrites.
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-lifecycle-prep.XXXXXX")
mmap_holder_pid=""

cleanup() {
    if [[ -n "$mmap_holder_pid" ]]; then
        kill -9 "$mmap_holder_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_root"
}
trap cleanup EXIT

# Source the lifecycle and state management libraries
source "$project_root/scripts/install_state.sh"
source "$project_root/scripts/install_lifecycle.sh"

# Set up fully isolated environment
export HOME="$test_root/home"
export XDG_CONFIG_HOME="$test_root/config"
export XDG_DATA_HOME="$test_root/data"
mkdir -p "$HOME" "$XDG_CONFIG_HOME/modernime" "$XDG_DATA_HOME/modernime"

prefix="$test_root/prefix"
desktop_dir="$test_root/Desktop"
payload="$test_root/payload"
transaction_dir="$test_root/transaction"

mkdir -p "$prefix" "$desktop_dir" "$payload" "$transaction_dir"

failures=0
assert_true() {
    local msg=$1
    shift
    if ! "$@"; then
        printf 'FAIL: %s (command: %s)\n' "$msg" "$*" >&2
        failures=$((failures + 1))
    else
        printf 'PASS: %s\n' "$msg"
    fi
}

# ==============================================================================
# Setup Initial Version (v0.1.7) in Prefix
# ==============================================================================
mkdir -p "$prefix/bin" "$prefix/lib/fcitx5" "$prefix/share/modernime/pinyin" "$prefix/unrelated"
printf 'executable-v0.1.7\n' > "$prefix/bin/modernime-settings"
printf 'ELF-plugin-v0.1.7\n' > "$prefix/lib/fcitx5/modernime_fcitx5.so"
printf 'dict-v0.1.7\n' > "$prefix/share/modernime/pinyin/knowledge.dict"
ln -s modernime-settings "$prefix/bin/modernime-link"
printf 'unrelated user file content\n' > "$prefix/unrelated/notes.txt"
unrelated_inode=$(stat -c '%i' "$prefix/unrelated/notes.txt")
old_so_inode=$(stat -c '%i' "$prefix/lib/fcitx5/modernime_fcitx5.so")

# Start background process holding mmap on running .so plugin (simulating Fcitx5)
python3 - "$prefix/lib/fcitx5/modernime_fcitx5.so" "$test_root/mmap.ready" "$test_root/mmap.stop" "$test_root/mmap.output" <<'PYEOF' &
import mmap, os, sys, time, pathlib
so_path, ready_path, stop_path, out_path = sys.argv[1:5]
fd = os.open(so_path, os.O_RDONLY)
mm = mmap.mmap(fd, 0, access=mmap.ACCESS_READ)
pathlib.Path(ready_path).write_text("ready\n")
while not pathlib.Path(stop_path).exists():
    time.sleep(0.05)
mm.seek(0)
pathlib.Path(out_path).write_bytes(mm.read())
mm.close()
os.close(fd)
PYEOF
mmap_holder_pid=$!
for _ in {1..50}; do
    [[ -f "$test_root/mmap.ready" ]] && break
    sleep 0.05
done
test -f "$test_root/mmap.ready"

# ==============================================================================
# Seed Initial User Data & Configuration
# ==============================================================================
printf 'candidate.page_size=5\ninput.toggle_key=Shift\n' > "$XDG_CONFIG_HOME/modernime/settings.conf"
printf 'chu\x27shi\t初始词条\t100\n' > "$XDG_DATA_HOME/modernime/user-dictionary.txt"

python3 - "$XDG_DATA_HOME/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
db_path = sys.argv[1]
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
conn.execute("""
    CREATE TABLE learning_entries (
        pinyin TEXT NOT NULL,
        phrase TEXT NOT NULL,
        frequency INTEGER NOT NULL DEFAULT 0,
        last_selected_ms INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (pinyin, phrase)
    );
""")
conn.execute("INSERT INTO learning_entries VALUES ('chu''shi', '初始学习词条', 10, 1700000000);")
conn.commit()
conn.close()
PYEOF

# ==============================================================================
# Prepare Payload for v0.1.8
# ==============================================================================
mkdir -p "$payload/bin" "$payload/lib/fcitx5" "$payload/share/modernime/pinyin" "$payload/share/applications"
printf 'executable-v0.1.8\n' > "$payload/bin/modernime-settings"
printf 'ELF-plugin-v0.1.8\n' > "$payload/lib/fcitx5/modernime_fcitx5.so"
printf 'dict-v0.1.8\n' > "$payload/share/modernime/pinyin/knowledge.dict"
printf 'new-hotwords-v0.1.8\n' > "$payload/share/modernime/pinyin/new-hotwords.dict"
printf '[Desktop Entry]\nName=ModernIME Settings\nExec=modernime-settings\n' > "$payload/share/applications/modernime-settings.desktop"

# ==============================================================================
# TEST 1: Core Assertion - RollbackPreservesConcurrentUserWrites
# ==============================================================================
printf '\n=== Running Test 1: RollbackPreservesConcurrentUserWrites ===\n'

# Start a concurrent writer in background that performs live learning commits
# and config changes during deployment
python3 - "$XDG_DATA_HOME/modernime/learning.sqlite3" "$XDG_CONFIG_HOME/modernime/settings.conf" "$test_root/writer.ready" <<'PYEOF' &
import sqlite3, sys, time, pathlib
db_path, config_path, ready_path = sys.argv[1:4]
# Simulate typing activity right as installation is under way
conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("INSERT INTO learning_entries VALUES ('bing''fa', '并发WAL词条', 15, 1700000001);")
conn.execute("INSERT INTO learning_entries VALUES ('xin''ci', '新学词条', 8, 1700000002);")
conn.commit()
conn.close()

# User updates config during installation window
with open(config_path, "w", encoding="utf-8") as f:
    f.write("candidate.page_size=9\ntheme=dark\ninput.toggle_key=Alt+Space\n")

pathlib.Path(ready_path).write_text("done\n")
PYEOF
writer_pid=$!

# Trigger deployment with simulated failure
set +e
MODERNIME_SIMULATE_INSTALL_FAILURE=1 \
    modernime_deploy_payload "$payload" "$prefix" "$transaction_dir"
deploy_exit_code=$?
set -e

wait "$writer_pid"
assert_true "Deployment with simulated failure exits non-zero" test "$deploy_exit_code" -ne 0

# Assertion 1.1: Verify SQLite WAL integrity and all concurrent writes preserved
python3 - "$XDG_DATA_HOME/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
assert conn.execute("PRAGMA integrity_check;").fetchone()[0] == "ok"
rows = conn.execute("SELECT phrase FROM learning_entries;").fetchall()
phrases = {r[0] for r in rows}
expected = {"初始学习词条", "并发WAL词条", "新学词条"}
assert phrases == expected, f"Expected {expected}, got {phrases}"
conn.close()
PYEOF
assert_true "Rollback preserved all concurrent SQLite WAL learning entries" test $? -eq 0

# Assertion 1.2: Verify user-modified settings.conf was NOT overwritten
assert_true "User config candidate.page_size=9 preserved" grep -qF 'candidate.page_size=9' "$XDG_CONFIG_HOME/modernime/settings.conf"
assert_true "User config theme=dark preserved" grep -qF 'theme=dark' "$XDG_CONFIG_HOME/modernime/settings.conf"
assert_true "User config input.toggle_key=Alt+Space preserved" grep -qF 'input.toggle_key=Alt+Space' "$XDG_CONFIG_HOME/modernime/settings.conf"

# Assertion 1.3: Verify user custom dictionary intact
assert_true "User custom dictionary preserved" grep -qF '初始词条' "$XDG_DATA_HOME/modernime/user-dictionary.txt"

# Assertion 1.4: Verify prefix files restored to v0.1.7
assert_true "Prefix executable restored to v0.1.7" grep -qF 'executable-v0.1.7' "$prefix/bin/modernime-settings"
assert_true "Prefix plugin restored to v0.1.7" grep -qF 'ELF-plugin-v0.1.7' "$prefix/lib/fcitx5/modernime_fcitx5.so"
assert_true "Prefix dictionary restored to v0.1.7" grep -qF 'dict-v0.1.7' "$prefix/share/modernime/pinyin/knowledge.dict"

# Assertion 1.5: Verify new payload files that failed mid-way were cleaned up
assert_true "New hotwords dict cleaned up on rollback" test ! -e "$prefix/share/modernime/pinyin/new-hotwords.dict"

# Assertion 1.6: Verify unrelated files in prefix remained untouched
assert_true "Unrelated user file preserved" grep -qF 'unrelated user file content' "$prefix/unrelated/notes.txt"
assert_true "Unrelated file inode unchanged" test "$(stat -c '%i' "$prefix/unrelated/notes.txt")" = "$unrelated_inode"

# Assertion 1.7: Verify transaction status logged as ROLLED_BACK
assert_true "Transaction status marked as ROLLED_BACK" grep -qF 'ROLLED_BACK' "$transaction_dir/status"

# ==============================================================================
# TEST 2: Pre-flight Backup Failure Aborts Before Touching Prefix
# ==============================================================================
printf '\n=== Running Test 2: Pre-flight Destination Collision Protection ===\n'
collision_transaction="$test_root/collision_tx"
mkdir -p "$collision_transaction"

# Create a collision: payload has file bin/collision_item, prefix has directory bin/collision_item
printf 'file content\n' > "$payload/bin/collision_item"
mkdir -p "$prefix/bin/collision_item"  # directory collision

set +e
modernime_deploy_payload "$payload" "$prefix" "$collision_transaction"
coll_exit_code=$?
set -e

assert_true "Destination collision aborts pre-flight" test "$coll_exit_code" -ne 0
assert_true "Collision status marked as FAILED_COLLISION" grep -qF 'FAILED_COLLISION' "$collision_transaction/status"
# Clean collision setup
rm -rf "$payload/bin/collision_item" "$prefix/bin/collision_item"

# ==============================================================================
# TEST 3: Clean Deployment Success & Unlink Protection for Active Libraries
# ==============================================================================
printf '\n=== Running Test 3: Clean Deployment Success ===\n'
success_transaction="$test_root/success_tx"
mkdir -p "$success_transaction"

modernime_deploy_payload "$payload" "$prefix" "$success_transaction"
assert_true "Clean deployment succeeds" test $? -eq 0
assert_true "Transaction status marked as COMMITTED" grep -qF 'COMMITTED' "$success_transaction/status"

# Verify files updated to v0.1.8
assert_true "Executable updated to v0.1.8" grep -qF 'executable-v0.1.8' "$prefix/bin/modernime-settings"
assert_true "Plugin updated to v0.1.8" grep -qF 'ELF-plugin-v0.1.8' "$prefix/lib/fcitx5/modernime_fcitx5.so"
assert_true "New hotwords dict deployed" grep -qF 'new-hotwords-v0.1.8' "$prefix/share/modernime/pinyin/new-hotwords.dict"

# Verify active mmap on old .so inode was preserved without crashing or corruption
touch "$test_root/mmap.stop"
wait "$mmap_holder_pid"
mmap_holder_pid=""
assert_true "Running process mmap was not corrupted" test "$(cat "$test_root/mmap.output")" = "ELF-plugin-v0.1.7"
new_so_inode=$(stat -c '%i' "$prefix/lib/fcitx5/modernime_fcitx5.so")
assert_true "Destination was unlinked: new inode deployed" test "$new_so_inode" != "$old_so_inode"

# ==============================================================================
# TEST 4: modernime_integrate Integration & User Modification Protection
# ==============================================================================
printf '\n=== Running Test 4: modernime_integrate & User Edit Protection ===\n'
managed_state_dir="$prefix/share/modernime/managed-files"
MODERNIME_INSTALLED_VERSION="0.1.8" \
    modernime_integrate "$prefix" "$XDG_CONFIG_HOME" "$desktop_dir" "$managed_state_dir"

assert_true "Desktop shortcut created" test -f "$desktop_dir/modernime-settings.desktop"
assert_true "Environment config created" test -f "$XDG_CONFIG_HOME/environment.d/90-modernime.conf"
assert_true "Autostart entry created" test -f "$XDG_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
assert_true "Install manifest created" test -f "$prefix/share/modernime/install-manifest.txt"
assert_true "Install channel file created" test -f "$prefix/share/modernime/install-channel.txt"
assert_true "Manifest contains desktop shortcut" grep -qF "$desktop_dir/modernime-settings.desktop" "$prefix/share/modernime/install-manifest.txt"
assert_true "Channel records version 0.1.8" grep -qF 'version=0.1.8' "$prefix/share/modernime/install-channel.txt"

# Simulate user modifying desktop file
printf '# User modified custom setting\n' >> "$desktop_dir/modernime-settings.desktop"
MODERNIME_INSTALLED_VERSION="0.1.8" \
    modernime_integrate "$prefix" "$XDG_CONFIG_HOME" "$desktop_dir" "$managed_state_dir"

assert_true "User edit to desktop shortcut preserved against re-integration" grep -qF '# User modified custom setting' "$desktop_dir/modernime-settings.desktop"

# ==============================================================================
# TEST 5: Prefix Lock Mutual Exclusion & Canonicalization (A12)
# ==============================================================================
printf '\n=== Running Test 5: Prefix Lock Mutual Exclusion & Canonicalization ===\n'

# 1. Canonicalization: symlink prefix and real prefix resolve to same lock
symlink_prefix="$test_root/symlink_prefix"
ln -s "$prefix" "$symlink_prefix"
canon_a=$(modernime_canonical_prefix "$prefix")
canon_b=$(modernime_canonical_prefix "$symlink_prefix")
assert_true "Canonical prefix matches between real and symlink" test "$canon_a" = "$canon_b"

# 2. Spaces in prefix path
space_prefix="$test_root/path with spaces/prefix"
mkdir -p "$(dirname -- "$space_prefix")"
canon_space=$(modernime_canonical_prefix "$space_prefix")
assert_true "Canonical prefix with spaces preserved" test "$canon_space" = "$space_prefix"

# 3. Mutual exclusion: two processes contending for same prefix
lock_test_prefix="$test_root/lock_target"
mkdir -p "$lock_test_prefix"

gate_fifo="$test_root/lock_gate"
rm -f "$gate_fifo"
mkfifo "$gate_fifo"

p1_script="
source '$project_root/scripts/install_lifecycle.sh'
if modernime_acquire_prefix_lock '$lock_test_prefix'; then
    printf 'p1_locked\n'
    read -r _ < '$gate_fifo'
    modernime_release_prefix_lock
else
    exit 2
fi
"
p1_out="$test_root/p1.out"
p1_err="$test_root/p1.err"
bash -c "$p1_script" >"$p1_out" 2>"$p1_err" &
p1_pid=$!

# Wait for p1 to acquire lock
for _ in {1..50}; do
    if grep -qF 'p1_locked' "$p1_out" 2>/dev/null; then
        break
    fi
    sleep 0.05
done
assert_true "Process 1 acquired prefix lock" grep -qF 'p1_locked' "$p1_out"

# Process 2 attempts non-blocking acquire on same prefix via symlink - MUST FAIL
symlink_prefix_for_lock="$test_root/symlink_lock_target"
ln -s "$lock_test_prefix" "$symlink_prefix_for_lock"

p2_script="
source '$project_root/scripts/install_lifecycle.sh'
if modernime_acquire_prefix_lock '$symlink_prefix_for_lock'; then
    printf 'p2_locked_unexpectedly\n'
    modernime_release_prefix_lock
    exit 0
else
    exit 3
fi
"
p2_out="$test_root/p2.out"
p2_err="$test_root/p2.err"
set +e
bash -c "$p2_script" >"$p2_out" 2>"$p2_err"
p2_exit=$?
set -e
assert_true "Process 2 was rejected due to lock contention (exit=3)" test "$p2_exit" -eq 3
assert_true "Process 2 reported locked prefix error" grep -qF 'locked by another ModernIME transaction' "$p2_err"

# Process 3 attempts acquire on DIFFERENT prefix - MUST SUCCEED concurrently
diff_prefix="$test_root/different_prefix"
mkdir -p "$diff_prefix"
p3_script="
source '$project_root/scripts/install_lifecycle.sh'
if modernime_acquire_prefix_lock '$diff_prefix'; then
    printf 'p3_locked\n'
    modernime_release_prefix_lock
    exit 0
else
    exit 4
fi
"
p3_out="$test_root/p3.out"
p3_err="$test_root/p3.err"
set +e
bash -c "$p3_script" >"$p3_out" 2>"$p3_err"
p3_exit=$?
set -e
assert_true "Different prefix acquired lock concurrently (exit=0)" test "$p3_exit" -eq 0

# Release Process 1
printf 'continue\n' > "$gate_fifo"
wait "$p1_pid"
assert_true "Process 1 released lock cleanly" test $? -eq 0

# 4. Re-entrancy in same process: repeated acquire does not deadlock or fail
assert_true "Acquire prefix lock once" modernime_acquire_prefix_lock "$lock_test_prefix"
assert_true "Re-entrant acquire succeeds without self-lock" modernime_acquire_prefix_lock "$lock_test_prefix"
assert_true "Release prefix lock" modernime_release_prefix_lock

# ==============================================================================
# TEST 6: Failed Rollback Recovery Material Retention (A11)
# ==============================================================================
printf '\n=== Running Test 6: Failed Rollback Recovery Material Retention ===\n'

failed_rb_tx="$test_root/failed_rb_tx"
mkdir -p "$failed_rb_tx/backup/bin"
printf 'old-settings\n' > "$failed_rb_tx/backup/bin/modernime-settings"
printf 'bin/modernime-settings\0' > "$failed_rb_tx/changed.list"
: > "$failed_rb_tx/created_dirs.list"
: > "$failed_rb_tx/transaction.log"

mkdir -p "$test_root/ro_prefix/bin"
printf 'new-broken\n' > "$test_root/ro_prefix/bin/modernime-settings"

rb_script="
source '$project_root/scripts/install_lifecycle.sh'
cp() {
    local src=\"\${@: -2:1}\"
    if [[ \"\$src\" == */backup/bin/modernime-settings ]]; then
        printf 'INJECTED: rollback restore failure\n' >&2
        return 1
    fi
    command cp \"\$@\"
}
modernime_rollback_payload '$test_root/ro_prefix' '$failed_rb_tx'
"
set +e
bash -c "$rb_script" >"$test_root/rb.out" 2>"$test_root/rb.err"
rb_exit=$?
set -e
assert_true "Rollback failure exits non-zero" test "$rb_exit" -ne 0
assert_true "Status marked as FAILED_ROLLBACK" grep -qF 'FAILED_ROLLBACK' "$failed_rb_tx/status"

# Test cleanup policy: modernime_cleanup_transaction_dir refuses to delete FAILED_ROLLBACK
set +e
cleanup_err="$test_root/cleanup.err"
modernime_cleanup_transaction_dir "$failed_rb_tx" 2>"$cleanup_err"
clean_exit=$?
set -e
assert_true "Cleanup refuses to remove FAILED_ROLLBACK (exit non-zero)" test "$clean_exit" -ne 0
assert_true "Transaction directory still exists on disk" test -d "$failed_rb_tx"
assert_true "Backup files preserved on disk" test -f "$failed_rb_tx/backup/bin/modernime-settings"
assert_true "Cleanup warning emitted" grep -qF 'ModernIME transaction rollback failed; recovery materials preserved' "$cleanup_err"

# ==============================================================================
# TEST 7: Integration Failure Status & Retention
# ==============================================================================
printf '\n=== Running Test 7: Integration Failure Status & Retention ===\n'

integ_fail_tx="$test_root/integ_fail_tx"
mkdir -p "$integ_fail_tx"
printf 'INTEGRATION_FAILED\n' > "$integ_fail_tx/status"
printf 'payload deployed\n' > "$integ_fail_tx/transaction.log"

set +e
integ_clean_err="$test_root/integ_cleanup.err"
modernime_cleanup_transaction_dir "$integ_fail_tx" 2>"$integ_clean_err"
integ_clean_exit=$?
set -e
assert_true "Cleanup refuses to remove INTEGRATION_FAILED tx dir" test "$integ_clean_exit" -ne 0
assert_true "Integration failed directory still exists" test -d "$integ_fail_tx"
assert_true "Warning emitted for failed integration" grep -qF 'ModernIME integration failed' "$integ_clean_err"

# ==============================================================================
# TEST 8: Generated Binary Installer Uses Prefix Lock & Safe Cleanup
# ==============================================================================
printf '\n=== Running Test 8: Generated Binary Installer Tests ===\n'

bin_pkg_dir="$test_root/gen_pkg"
mkdir -p "$bin_pkg_dir/usr"
/bin/cp -a "$payload/." "$bin_pkg_dir/usr/"
bash "$project_root/scripts/generate_binary_installer.sh" "$bin_pkg_dir"
assert_true "Generated install.sh exists in package" test -f "$bin_pkg_dir/install.sh"
assert_true "Generated install.sh uses modernime_acquire_prefix_lock" grep -qF 'modernime_acquire_prefix_lock' "$bin_pkg_dir/install.sh"
assert_true "Generated install.sh uses modernime_cleanup_transaction_dir" grep -qF 'modernime_cleanup_transaction_dir' "$bin_pkg_dir/install.sh"

# ==============================================================================
# TEST 9: Extension Packaging Whitelist With Dirty Files in Source (AR09 / A14)
# ==============================================================================
printf '\n=== Running Test 9: Extension Packaging Whitelist in Isolated Source ===\n'

iso_src="$test_root/iso_src"
iso_build="$test_root/iso_build"
iso_stage="$test_root/iso_stage"
mkdir -p "$iso_src/ext" "$iso_build" "$iso_stage"

# Copy real extension files to isolated source
/bin/cp -a "$project_root/data/extensions/kimpanel@kde.org/." "$iso_src/ext/"

# Inject dirty uncommitted files and undeclared files in isolated source
mkdir -p "$iso_src/ext/.mimosa" "$iso_src/ext/.v2c" "$iso_src/ext/schemas/.mimosa"
printf 'dirty-mimosa\n' > "$iso_src/ext/.mimosa/leak.txt"
printf 'dirty-v2c\n' > "$iso_src/ext/.v2c/temp.cache"
printf 'undeclared file\n' > "$iso_src/ext/scratch.log"
printf 'nested-mimosa\n' > "$iso_src/ext/schemas/.mimosa/bad.xml"

# Create minimal CMakeLists.txt using the same whitelist rules
cat > "$iso_src/CMakeLists.txt" << 'EOF'
cmake_minimum_required(VERSION 3.16)
project(TestExtWhitelist LANGUAGES NONE)
include(GNUInstallDirs)
set(MODERNIME_KIMPANEL_ROOT_FILES
    extension.js indicator.js lib.js menu.js metadata.json panel.js prefs.js stylesheet.css
)
foreach(file IN LISTS MODERNIME_KIMPANEL_ROOT_FILES)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ext/${file}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/extensions/kimpanel@kde.org")
endforeach()
set(MODERNIME_KIMPANEL_SCHEMAS_FILES
    gschemas.compiled org.gnome.shell.extensions.kimpanel.gschema.xml
)
foreach(file IN LISTS MODERNIME_KIMPANEL_SCHEMAS_FILES)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ext/schemas/${file}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/extensions/kimpanel@kde.org/schemas")
endforeach()
set(MODERNIME_KIMPANEL_LOCALES
    de es fr he ja ko nl ru zh_CN zh_TW
)
foreach(loc IN LISTS MODERNIME_KIMPANEL_LOCALES)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ext/locale/${loc}/LC_MESSAGES/gnome-shell-extensions-kimpanel.mo"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/modernime/extensions/kimpanel@kde.org/locale/${loc}/LC_MESSAGES")
endforeach()
EOF

cmake -S "$iso_src" -B "$iso_build" >/dev/null
cmake --install "$iso_build" --prefix "$iso_stage" >/dev/null

installed_ext="$iso_stage/share/modernime/extensions/kimpanel@kde.org"
assert_true "Extension metadata exists in staging" test -f "$installed_ext/metadata.json"
assert_true "Extension schemas exist in staging" test -f "$installed_ext/schemas/gschemas.compiled"
assert_true "Extension locales exist in staging" test -f "$installed_ext/locale/zh_CN/LC_MESSAGES/gnome-shell-extensions-kimpanel.mo"

installed_count=$(find "$installed_ext" -type f | wc -l)
assert_true "Exactly 20 whitelist extension files installed" test "$installed_count" -eq 20

dirty_count=$(find "$iso_stage" \( -name "*mimosa*" -o -name "*v2c*" -o -name "scratch.log" \) | wc -l)
assert_true "Zero dirty/undeclared files present in staging" test "$dirty_count" -eq 0

# ==============================================================================
# TEST 10: Dynamic Staging Manifest Generation & Obsolete Payload Pruning (AR15 / D04)
# ==============================================================================
printf '\n=== Running Test 10: Dynamic Manifest & Obsolete Payload Pruning ===\n'

ar15_root="$test_root/ar15"
ar15_prefix="$ar15_root/prefix"
ar15_config="$ar15_root/config"
ar15_desktop="$ar15_root/Desktop"
ar15_data="$ar15_root/data"
ar15_managed="$ar15_prefix/share/modernime/managed-files"
mkdir -p "$ar15_prefix" "$ar15_config" "$ar15_desktop" "$ar15_data"

# Phase A: Deploy V1 with obsolete-item.txt
v1_payload="$ar15_root/v1_payload"
mkdir -p "$v1_payload/bin" "$v1_payload/share/modernime/pinyin"
printf 'settings-v1\n' > "$v1_payload/bin/modernime-settings"
printf 'knowledge-v1\n' > "$v1_payload/share/modernime/pinyin/modernime-knowledge.dict"
printf 'obsolete-feature-data\n' > "$v1_payload/share/modernime/obsolete-item.txt"

v1_tx="$ar15_root/v1_tx"
mkdir -p "$v1_tx"
assert_true "Deploy v1 payload" modernime_deploy_payload "$v1_payload" "$ar15_prefix" "$v1_tx"
assert_true "Integrate v1 payload" modernime_integrate "$ar15_prefix" "$ar15_config" "$ar15_desktop" "$ar15_managed" "$ar15_data" "$v1_tx/deployed_files.list"
modernime_cleanup_transaction_dir "$v1_tx"

assert_true "V1 obsolete item on disk" test -f "$ar15_prefix/share/modernime/obsolete-item.txt"
assert_true "V1 manifest contains obsolete item" grep -qF "$ar15_prefix/share/modernime/obsolete-item.txt" "$ar15_prefix/share/modernime/install-manifest.txt"
assert_true "V1 payload.manifest written" test -f "$ar15_managed/payload.manifest"

# Phase B: Deploy V2 which drops obsolete-item.txt and adds new-item.txt
v2_payload="$ar15_root/v2_payload"
mkdir -p "$v2_payload/bin" "$v2_payload/share/modernime/pinyin"
printf 'settings-v2\n' > "$v2_payload/bin/modernime-settings"
printf 'knowledge-v2\n' > "$v2_payload/share/modernime/pinyin/modernime-knowledge.dict"
printf 'new-feature-data\n' > "$v2_payload/share/modernime/new-item.txt"

v2_tx="$ar15_root/v2_tx"
mkdir -p "$v2_tx"
assert_true "Deploy v2 payload" modernime_deploy_payload "$v2_payload" "$ar15_prefix" "$v2_tx"
assert_true "Integrate v2 payload" modernime_integrate "$ar15_prefix" "$ar15_config" "$ar15_desktop" "$ar15_managed" "$ar15_data" "$v2_tx/deployed_files.list"
modernime_cleanup_transaction_dir "$v2_tx"

assert_true "Obsolete item pruned on v2 upgrade" test ! -f "$ar15_prefix/share/modernime/obsolete-item.txt"
assert_true "New item deployed on v2 upgrade" test -f "$ar15_prefix/share/modernime/new-item.txt"
assert_true "V2 manifest contains new item" grep -qF "$ar15_prefix/share/modernime/new-item.txt" "$ar15_prefix/share/modernime/install-manifest.txt"
assert_true "V2 manifest does not contain pruned item" test ! $(grep -c "$ar15_prefix/share/modernime/obsolete-item.txt" "$ar15_prefix/share/modernime/install-manifest.txt") -gt 0

# ==============================================================================
# TEST 11: Uninstall Trust Boundary & Security Path Verification (AR15 / D04)
# ==============================================================================
printf '\n=== Running Test 11: Uninstall Trust Boundary & Security Checks ===\n'

manifest_file="$ar15_prefix/share/modernime/install-manifest.txt"
manifest_backup="$ar15_root/manifest.valid"
cp "$manifest_file" "$manifest_backup"

# 1. Path traversal injection
printf '%s/share/modernime/../../evil.txt\n' "$ar15_prefix" > "$manifest_file"
set +e
traversal_err="$ar15_root/traversal.err"
HOME="$test_root/home" XDG_CONFIG_HOME="$ar15_config" MODERNIME_PREFIX="$ar15_prefix" \
    bash "$project_root/uninstall.sh" --prefix="$ar15_prefix" >/dev/null 2>"$traversal_err"
traversal_exit=$?
set -e
assert_true "Uninstall rejects path traversal (exit non-zero)" test "$traversal_exit" -ne 0
assert_true "Error message emitted for traversal" grep -qF 'Refusing unexpected manifest path' "$traversal_err"

# 2. External unmanaged path injection
printf '%s/unmanaged-system-file.txt\n' "$test_root" > "$manifest_file"
set +e
external_err="$ar15_root/external.err"
HOME="$test_root/home" XDG_CONFIG_HOME="$ar15_config" MODERNIME_PREFIX="$ar15_prefix" \
    bash "$project_root/uninstall.sh" --prefix="$ar15_prefix" >/dev/null 2>"$external_err"
external_exit=$?
set -e
assert_true "Uninstall rejects external unmanaged path (exit non-zero)" test "$external_exit" -ne 0
assert_true "Error message emitted for external unmanaged path" grep -qF 'Refusing unexpected manifest path' "$external_err"

# 3. Prefix string camouflage injection (starts with prefix string but outside prefix directory)
printf '%s_evil/bin/modernime-settings\n' "$ar15_prefix" > "$manifest_file"
set +e
camou_err="$ar15_root/camou.err"
HOME="$test_root/home" XDG_CONFIG_HOME="$ar15_config" MODERNIME_PREFIX="$ar15_prefix" \
    bash "$project_root/uninstall.sh" --prefix="$ar15_prefix" >/dev/null 2>"$camou_err"
camou_exit=$?
set -e
assert_true "Uninstall rejects prefix string camouflage (exit non-zero)" test "$camou_exit" -ne 0
assert_true "Error message emitted for camouflage" grep -qF 'Refusing unexpected manifest path' "$camou_err"

# 4. Unrelated binary inside prefix injection
printf '%s/bin/some_other_daemon\n' "$ar15_prefix" > "$manifest_file"
set +e
unowned_err="$ar15_root/unowned.err"
HOME="$test_root/home" XDG_CONFIG_HOME="$ar15_config" MODERNIME_PREFIX="$ar15_prefix" \
    bash "$project_root/uninstall.sh" --prefix="$ar15_prefix" >/dev/null 2>"$unowned_err"
unowned_exit=$?
set -e
assert_true "Uninstall rejects unowned namespace file in prefix (exit non-zero)" test "$unowned_exit" -ne 0
assert_true "Error message emitted for unowned file" grep -qF 'Refusing unexpected manifest path' "$unowned_err"

# 5. Restore valid manifest and execute clean uninstall
cp "$manifest_backup" "$manifest_file"
assert_true "Clean uninstall succeeds" \
    env HOME="$test_root/home" XDG_CONFIG_HOME="$ar15_config" MODERNIME_PREFIX="$ar15_prefix" \
    bash "$project_root/uninstall.sh" --prefix="$ar15_prefix"

assert_true "Settings binary removed" test ! -f "$ar15_prefix/bin/modernime-settings"
assert_true "New item removed" test ! -f "$ar15_prefix/share/modernime/new-item.txt"
assert_true "Managed files state directory cleaned up" test ! -d "$ar15_managed"
assert_true "Manifest file cleaned up" test ! -f "$manifest_file"

# ==============================================================================
# Final Test Summary
# ==============================================================================
printf '\n============================================================\n'
if [[ "$failures" -eq 0 ]]; then
    printf 'ALL PREPARATION LIFECYCLE FIXTURE TESTS PASSED (0 failures)\n'
    exit 0
else
    printf 'TESTS FAILED: %d assertions failed\n' "$failures" >&2
    exit 1
fi
