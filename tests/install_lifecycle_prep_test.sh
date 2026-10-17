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
