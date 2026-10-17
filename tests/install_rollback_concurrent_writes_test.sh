#!/usr/bin/env bash
# tests/install_rollback_concurrent_writes_test.sh
#
# T17 Verification Fixture: RollbackPreservesConcurrentUserWrites and Baseline Anti-Example
#
# Contract:
# 1. During an upgrade/install attempt, concurrent user writes (learning store in WAL mode,
#    user configuration edits, custom dictionary additions) may take place simultaneously.
# 2. When an installation failure occurs at any phase (staging, backup, payload deployment,
#    or integration) and triggers rollback:
#    - The payload files in PREFIX must be rolled back cleanly to the pre-upgrade state.
#    - CRITICALLY: All concurrent user writes made during the installation attempt MUST be
#      100% PRESERVED. The installer must NEVER overwrite, delete, or restore a pre-upgrade
#      snapshot over live user databases or user configurations.
# 3. Baseline Anti-Example: A flawed rollback implementation that snapshots and restores
#    user data directories will clobber concurrent user writes. We prove that this fixture
#    detects such regressions and fails when presented with such flawed rollback behavior.

set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-rollback-concurrent.XXXXXX")

cleanup() {
    local exit_code=$?
    # Terminate any lingering background worker if pid is still active
    if [[ -n "${concurrent_writer_pid:-}" ]]; then
        kill -9 "$concurrent_writer_pid" 2>/dev/null || true
    fi
    rm -rf -- "$test_root"
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

fake_bin="$test_root/fake-bin"
mkdir -p "$fake_bin"

for cmd in ctest fcitx5 fcitx5-remote pkg-config; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$fake_bin/$cmd"
done
chmod +x "$fake_bin"/*

export PATH="$fake_bin:$PATH"

log_info() {
    printf '[INFO] %s\n' "$*"
}

log_error() {
    printf '[ERROR] %s\n' "$*" >&2
}

# -----------------------------------------------------------------------------
# Helper: Setup mock pre-existing v0.1.7 installation in target prefix
# -----------------------------------------------------------------------------
setup_prefix_v017() {
    local prefix=$1
    mkdir -p "$prefix/bin" \
             "$prefix/lib/x86_64-linux-gnu/fcitx5" \
             "$prefix/share/modernime/pinyin" \
             "$prefix/share/modernime/managed-files" \
             "$prefix/share/fcitx5/addon" \
             "$prefix/share/fcitx5/inputmethod" \
             "$prefix/share/applications"

    cat > "$prefix/bin/modernime-settings" <<'EOF'
#!/usr/bin/env bash
echo "modernime-settings 0.1.7"
EOF
    chmod +x "$prefix/bin/modernime-settings"

    printf 'ELF-v0.1.7-fcitx5-plugin\n' > "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
    printf 'ELF-v0.1.7-ui-plugin\n' > "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"
    printf 'dict-v0.1.7\n' > "$prefix/share/modernime/pinyin/modernime-knowledge.dict"
    printf 'channel=script\nversion=0.1.7\nprefix=%s\n' "$prefix" > "$prefix/share/modernime/install-channel.txt"

    # Also record an unowned unrelated user file that must never be touched
    mkdir -p "$prefix/unrelated"
    printf 'unrelated user file content\n' > "$prefix/unrelated/notes.txt"
}

# -----------------------------------------------------------------------------
# Helper: Setup user data environment (SQLite in WAL mode, config, user dict)
# -----------------------------------------------------------------------------
setup_user_data() {
    local data_home=$1
    local config_home=$2

    mkdir -p "$data_home/modernime" "$config_home/modernime"

    # Seed initial configuration
    cat > "$config_home/modernime/settings.conf" <<'EOF'
input.enabled=true
input.toggle_key=Shift
candidate.font_size=14
candidate.page_size=5
EOF

    # Seed initial user dictionary
    cat > "$data_home/modernime/user-dictionary.txt" <<'EOF'
chu'shi	初始词条	100
EOF

    # Seed initial SQLite database in WAL mode
    python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
conn.execute("""
    CREATE TABLE learning_entries (
        pinyin TEXT NOT NULL,
        phrase TEXT NOT NULL,
        context_before TEXT NOT NULL DEFAULT '',
        context_after TEXT NOT NULL DEFAULT '',
        frequency INTEGER NOT NULL DEFAULT 0,
        last_selected_ms INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (pinyin, phrase, context_before, context_after)
    );
""")
conn.execute("INSERT INTO learning_entries VALUES ('ni''hao', '初始你好', '', '', 10, 1700000000);")
conn.commit()
conn.close()
PYEOF
}

# -----------------------------------------------------------------------------
# Helper: Verify user data integrity and presence of concurrent writes
# Returns 0 if all initial + concurrent writes are intact; non-zero otherwise.
# -----------------------------------------------------------------------------
verify_concurrent_writes() {
    local data_home=$1
    local config_home=$2
    local expected_concurrent_phrase=${3:-"并发学习词条"}
    local expected_config_fragment=${4:-"candidate.font_size=20"}

    # 1. Verify user configuration preserves concurrent edits
    if ! grep -qF "$expected_config_fragment" "$config_home/modernime/settings.conf"; then
        log_error "Concurrent configuration edit not found in settings.conf"
        return 1
    fi

    # 2. Verify custom dictionary preserves concurrent entries
    if ! grep -qF "并发词条" "$data_home/modernime/user-dictionary.txt"; then
        log_error "Concurrent dictionary entry not found in user-dictionary.txt"
        return 1
    fi

    # 3. Verify SQLite integrity and presence of both initial and concurrent writes
    python3 - "$data_home/modernime/learning.sqlite3" "$expected_concurrent_phrase" <<'PYEOF'
import sqlite3, sys
db_path = sys.argv[1]
expected_phrase = sys.argv[2]
conn = sqlite3.connect(db_path)
integrity = conn.execute("PRAGMA integrity_check;").fetchone()[0]
if integrity != "ok":
    print(f"SQLite integrity check failed: {integrity}", file=sys.stderr)
    sys.exit(1)

phrases = {r[0] for r in conn.execute("SELECT phrase FROM learning_entries;").fetchall()}
if "初始你好" not in phrases:
    print(f"Missing initial entry '初始你好'; found: {phrases}", file=sys.stderr)
    sys.exit(1)

if expected_phrase not in phrases:
    print(f"Missing concurrent entry '{expected_phrase}'; found: {phrases}", file=sys.stderr)
    sys.exit(1)

conn.close()
PYEOF
}

# -----------------------------------------------------------------------------
# TEST 1: Baseline Anti-Example (基线反例验证)
# Demonstrates that a flawed installer that snapshots and restores user data
# on failure will destroy concurrent user writes, and proves our verification
# function detects this regression.
# -----------------------------------------------------------------------------
test_baseline_anti_example() {
    log_info "Running Test 1: Baseline Anti-Example (Flawed Rollback Overwrites User Data)..."
    local case_dir="$test_root/baseline_anti_example"
    local prefix="$case_dir/prefix"
    local data_home="$case_dir/data"
    local config_home="$case_dir/config"

    setup_prefix_v017 "$prefix"
    setup_user_data "$data_home" "$config_home"

    # Flawed installer logic:
    # Step 1: Takes pre-install snapshot of user data
    local user_snapshot_dir="$case_dir/flawed_user_snapshot"
    mkdir -p "$user_snapshot_dir"
    cp -a "$data_home/." "$user_snapshot_dir/data/"
    cp -a "$config_home/." "$user_snapshot_dir/config/"

    # Step 2: User performs concurrent writes while install is "in flight"
    python3 - "$data_home/modernime/learning.sqlite3" <<'PYEOF'
import sqlite3, sys
conn = sqlite3.connect(sys.argv[1])
conn.execute("INSERT INTO learning_entries VALUES ('bing''fa', '并发学习词条', '', '', 5, 1700000010);")
conn.commit()
conn.close()
PYEOF
    echo "candidate.font_size=20" >> "$config_home/modernime/settings.conf"
    echo -e "bing'fa\t并发词条\t99" >> "$data_home/modernime/user-dictionary.txt"

    # Step 3: Installation encounters an error and executes FLAWED rollback
    # (naive rollback restores the pre-install user data snapshot)
    cp -a --remove-destination "$user_snapshot_dir/data/." "$data_home/"
    cp -a --remove-destination "$user_snapshot_dir/config/." "$config_home/"

    # Step 4: Run verify_concurrent_writes. It MUST detect the loss and return non-zero!
    if verify_concurrent_writes "$data_home" "$config_home" >/dev/null 2>&1; then
        log_error "Anti-example failed: Flawed rollback was NOT detected!"
        return 1
    fi

    log_info "Pass: Baseline Anti-Example correctly demonstrated and caught data loss from flawed rollback."
    return 0
}

# -----------------------------------------------------------------------------
# TEST 2: RollbackPreservesConcurrentUserWrites with Space Prefix
# Simulates concurrent user activity during an installation that fails and
# asserts that all user data is 100% preserved while prefix binaries are rolled back.
# -----------------------------------------------------------------------------
test_rollback_preserves_concurrent_user_writes() {
    log_info "Running Test 2: RollbackPreservesConcurrentUserWrites (Space Prefix & Active WAL)..."
    local case_dir="$test_root/concurrent_rollback_case"
    local prefix="$case_dir/modernime test prefix with spaces"
    local data_home="$case_dir/user data home with spaces"
    local config_home="$case_dir/user config home with spaces"
    local transaction_dir="$case_dir/trans_dir"

    setup_prefix_v017 "$prefix"
    setup_user_data "$data_home" "$config_home"
    mkdir -p "$transaction_dir/backup"

    local unrelated_inode
    unrelated_inode=$(stat -c %i "$prefix/unrelated/notes.txt")

    # Simulate staged payload for v0.1.8
    local stage_dir="$case_dir/staged_v018"
    mkdir -p "$stage_dir/bin" "$stage_dir/lib/x86_64-linux-gnu/fcitx5" "$stage_dir/share/modernime/pinyin"
    cat > "$stage_dir/bin/modernime-settings" <<'EOF'
#!/usr/bin/env bash
echo "modernime-settings 0.1.8"
EOF
    chmod +x "$stage_dir/bin/modernime-settings"
    printf 'ELF-v0.1.8-fcitx5-plugin\n' > "$stage_dir/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
    printf 'new-dict-v0.1.8\n' > "$stage_dir/share/modernime/pinyin/modernime-knowledge.dict"

    # Start background concurrent worker that continuously commits WAL frames and config edits
    local stop_file="$case_dir/worker.stop"
    local ready_file="$case_dir/worker.ready"

    python3 - "$data_home/modernime/learning.sqlite3" "$ready_file" "$stop_file" <<'PYEOF' &
import sqlite3, sys, time, pathlib
db_path = sys.argv[1]
ready_path = pathlib.Path(sys.argv[2])
stop_path = pathlib.Path(sys.argv[3])

conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
ready_path.write_text("ready\n")

counter = 0
while not stop_path.exists():
    counter += 1
    conn.execute(
        "INSERT OR REPLACE INTO learning_entries VALUES (?, ?, '', '', ?, ?);",
        (f"bing'{counter}", "并发学习词条", counter, int(time.time()))
    )
    conn.commit()
    time.sleep(0.02)

conn.close()
PYEOF
    concurrent_writer_pid=$!

    # Wait for writer to be active
    for _ in {1..50}; do
        [[ -f "$ready_file" ]] && break
        sleep 0.02
    done
    test -f "$ready_file"

    # Concurrent config and dict modification
    echo "candidate.font_size=20" >> "$config_home/modernime/settings.conf"
    echo -e "bing'fa\t并发词条\t99" >> "$data_home/modernime/user-dictionary.txt"

    # Execute atomic deployment with injected failure:
    # Backup existing colliding files in prefix
    local changed_list="$transaction_dir/changed.txt"
    : > "$changed_list"

    while IFS= read -r -d '' src_file; do
        rel_path="${src_file#"$stage_dir/"}"
        dest_file="$prefix/$rel_path"
        if [[ -e "$dest_file" || -L "$dest_file" ]]; then
            mkdir -p "$transaction_dir/backup/$(dirname "$rel_path")"
            cp -a -- "$dest_file" "$transaction_dir/backup/$rel_path"
        fi
    done < <(find "$stage_dir" \( -type f -o -type l \) -print0)

    # Perform deployment and inject failure halfway
    local deploy_failed=false
    while IFS= read -r -d '' src_file; do
        rel_path="${src_file#"$stage_dir/"}"
        dest_file="$prefix/$rel_path"
        mkdir -p "$(dirname "$dest_file")"
        printf '%s\0' "$rel_path" >> "$changed_list"
        cp -a --remove-destination -- "$src_file" "$dest_file"

        # Injected failure trigger after copying first file
        if [[ "$rel_path" == "bin/modernime-settings" ]]; then
            deploy_failed=true
            break
        fi
    done < <(find "$stage_dir" \( -type f -o -type l \) -print0)

    test "$deploy_failed" = true

    # Execute correct rollback:
    # 1. Remove changed files
    while IFS= read -r -d '' rel_path; do
        dest_file="$prefix/$rel_path"
        rm -f -- "$dest_file"
        if [[ -e "$transaction_dir/backup/$rel_path" || -L "$transaction_dir/backup/$rel_path" ]]; then
            cp -a --remove-destination -- "$transaction_dir/backup/$rel_path" "$dest_file"
        fi
    done < "$changed_list"

    # 2. Stop concurrent worker
    touch "$stop_file"
    wait "$concurrent_writer_pid"
    unset concurrent_writer_pid

    # 3. Assertions on prefix recovery:
    grep -qF '0.1.7' "$prefix/bin/modernime-settings"
    test "$(cat "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so")" = "ELF-v0.1.7-fcitx5-plugin"
    test "$(stat -c %i "$prefix/unrelated/notes.txt")" = "$unrelated_inode"

    # 4. Critical assertion: verify all concurrent user writes are preserved!
    if ! verify_concurrent_writes "$data_home" "$config_home"; then
        log_error "Test 2 failed: Concurrent writes were not preserved after rollback!"
        return 1
    fi

    log_info "Pass: Test 2 (RollbackPreservesConcurrentUserWrites) verified successfully."
    return 0
}

# -----------------------------------------------------------------------------
# TEST 3: Exercise scripts/install_lifecycle.sh modernime_deploy_payload Directly
# Injects failure via MODERNIME_SIMULATE_INSTALL_FAILURE=1 and verifies that
# modernime_deploy_payload rolls back prefix cleanly while concurrent user writes
# remain 100% intact.
# -----------------------------------------------------------------------------
test_lifecycle_library_deploy_rollback_with_concurrent_writes() {
    log_info "Running Test 3: modernime_deploy_payload Lifecycle Rollback under Concurrent Writes..."
    local lifecycle_lib="$project_root/scripts/install_lifecycle.sh"
    test -f "$lifecycle_lib"
    source "$lifecycle_lib"

    local case_dir="$test_root/lifecycle_lib_case"
    local prefix="$case_dir/prefix space"
    local data_home="$case_dir/data space"
    local config_home="$case_dir/config space"
    local transaction_dir="$case_dir/trans space"

    setup_prefix_v017 "$prefix"
    setup_user_data "$data_home" "$config_home"
    mkdir -p "$transaction_dir"

    local unrelated_inode
    unrelated_inode=$(stat -c %i "$prefix/unrelated/notes.txt")

    # Staged payload v0.1.8
    local stage_dir="$case_dir/staged_v018"
    mkdir -p "$stage_dir/bin" "$stage_dir/lib/x86_64-linux-gnu/fcitx5" "$stage_dir/share/modernime/pinyin"
    cat > "$stage_dir/bin/modernime-settings" <<'EOF'
#!/usr/bin/env bash
echo "modernime-settings 0.1.8"
EOF
    chmod +x "$stage_dir/bin/modernime-settings"
    printf 'ELF-v0.1.8-fcitx5-plugin\n' > "$stage_dir/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
    printf 'new-dict-v0.1.8\n' > "$stage_dir/share/modernime/pinyin/modernime-knowledge.dict"

    # Start background concurrent worker
    local stop_file="$case_dir/worker.stop"
    local ready_file="$case_dir/worker.ready"

    python3 - "$data_home/modernime/learning.sqlite3" "$ready_file" "$stop_file" <<'PYEOF' &
import sqlite3, sys, time, pathlib
db_path = sys.argv[1]
ready_path = pathlib.Path(sys.argv[2])
stop_path = pathlib.Path(sys.argv[3])

conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL;")
conn.execute("PRAGMA wal_autocheckpoint=0;")
ready_path.write_text("ready\n")

counter = 0
while not stop_path.exists():
    counter += 1
    conn.execute(
        "INSERT OR REPLACE INTO learning_entries VALUES (?, ?, '', '', ?, ?);",
        (f"bing'{counter}", "并发学习词条", counter, int(time.time()))
    )
    conn.commit()
    time.sleep(0.02)

conn.close()
PYEOF
    concurrent_writer_pid=$!

    for _ in {1..50}; do
        [[ -f "$ready_file" ]] && break
        sleep 0.02
    done
    test -f "$ready_file"

    echo "candidate.font_size=20" >> "$config_home/modernime/settings.conf"
    echo -e "bing'fa\t并发词条\t99" >> "$data_home/modernime/user-dictionary.txt"

    # Call modernime_deploy_payload with simulated failure
    local call_status=0
    MODERNIME_SIMULATE_INSTALL_FAILURE=1 modernime_deploy_payload "$stage_dir" "$prefix" "$transaction_dir" || call_status=$?

    # Stop concurrent worker
    touch "$stop_file"
    wait "$concurrent_writer_pid"
    unset concurrent_writer_pid

    if [[ "$call_status" -eq 0 ]]; then
        log_error "Test 3 failed: modernime_deploy_payload unexpectedly succeeded despite MODERNIME_SIMULATE_INSTALL_FAILURE=1"
        return 1
    fi

    # Assert transaction status is ROLLED_BACK
    local final_status
    final_status=$(cat "$transaction_dir/status")
    test "$final_status" = "ROLLED_BACK"

    # Assert prefix files restored to v0.1.7
    grep -qF '0.1.7' "$prefix/bin/modernime-settings"
    test "$(cat "$prefix/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so")" = "ELF-v0.1.7-fcitx5-plugin"
    test "$(stat -c %i "$prefix/unrelated/notes.txt")" = "$unrelated_inode"
    test ! -f "$prefix/share/modernime/pinyin/modernime-knowledge.dict" || test "$(cat "$prefix/share/modernime/pinyin/modernime-knowledge.dict")" = "dict-v0.1.7"

    # Assert concurrent user writes preserved
    if ! verify_concurrent_writes "$data_home" "$config_home"; then
        log_error "Test 3 failed: Concurrent writes were not preserved after modernime_deploy_payload rollback!"
        return 1
    fi

    log_info "Pass: Test 3 (modernime_deploy_payload Lifecycle Rollback) verified successfully."
    return 0
}

# -----------------------------------------------------------------------------
# Main Execution
# -----------------------------------------------------------------------------
main() {
    log_info "Starting T17 installation failure & rollback fixtures..."
    test_baseline_anti_example
    test_rollback_preserves_concurrent_user_writes
    test_lifecycle_library_deploy_rollback_with_concurrent_writes
    log_info "All T17 preparation rollback fixtures executed successfully!"
}

main "$@"
