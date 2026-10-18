#!/usr/bin/env bash
# ModernIME T18: Isolated DBus Navigation & Target Activation Test
set -euo pipefail

# 1. Four-grid matrix Step 1: Validate required binary presence and executability
# Missing or unexecutable binary MUST exit 1 regardless of display environment
SETTINGS_BIN="${1:-${SETTINGS_BIN:-}}"
if [ -z "$SETTINGS_BIN" ]; then
    echo "[ERROR] Missing required settings binary argument (exit 1)" >&2
    exit 1
fi

if [ ! -x "$SETTINGS_BIN" ]; then
    echo "[ERROR] Required binary '$SETTINGS_BIN' does not exist or is not executable (exit 1)" >&2
    exit 1
fi

# 2. Four-grid matrix Step 2: Headless / Display Detection
# Only when binary is valid AND display is unavailable do we return exit 77
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "[SKIP] No graphical display available for GTK DBus navigation test (exit 77)"
    exit 77
fi

# 3. Create isolated sandbox environment
SANDBOX_DIR=$(mktemp -d "/tmp/modernime_dbus_t18_XXXXXX")
cleanup_dir() {
    local dir="$1"
    if [ -d "$dir" ]; then
        fusermount -u "$dir/runtime/doc" 2>/dev/null || true
        fusermount -u "$dir/runtime/gvfs" 2>/dev/null || true
        fusermount -uz "$dir/runtime/doc" 2>/dev/null || true
        fusermount -uz "$dir/runtime/gvfs" 2>/dev/null || true
        rm -rf "$dir" 2>/dev/null || true
    fi
}

cleanup() {
    if [ -n "${PRIMARY_PID:-}" ] && kill -0 "$PRIMARY_PID" 2>/dev/null; then
        kill "$PRIMARY_PID" 2>/dev/null || true
        wait "$PRIMARY_PID" 2>/dev/null || true
    fi
    cleanup_dir "$SANDBOX_DIR"
}
trap cleanup EXIT

export XDG_CONFIG_HOME="$SANDBOX_DIR/config"
export XDG_DATA_HOME="$SANDBOX_DIR/data"
export XDG_RUNTIME_DIR="$SANDBOX_DIR/runtime"
export HOME="$SANDBOX_DIR/home"
mkdir -p "$XDG_CONFIG_HOME/modernime" "$XDG_DATA_HOME/modernime" "$XDG_RUNTIME_DIR" "$HOME"
chmod 0700 "$XDG_RUNTIME_DIR"

# 4. Helper function to run the full navigation verification suite against a target executable
run_suite() {
    local TARGET_BIN="$1"
    local SUB_SANDBOX
    SUB_SANDBOX=$(mktemp -d "$SANDBOX_DIR/suite_run_XXXXXX")
    local STATE_FILE="$SUB_SANDBOX/state.json"

    export MODERNIME_SETTINGS_STATE_FILE="$STATE_FILE"

    local suite_script="$SUB_SANDBOX/run_suite.sh"
    cat > "$suite_script" << 'SUITE_EOF'
#!/usr/bin/env bash
set -euo pipefail

TARGET_BIN="$1"
STATE_FILE="$2"

read_state() {
    local key="$1"
    if [ ! -f "$STATE_FILE" ]; then
        echo ""
        return
    fi
    python3 -c "import json; data = json.load(open('$STATE_FILE')); print(data.get('$key', ''))" 2>/dev/null || echo ""
}

wait_for_condition() {
    local key="$1"
    local expected="$2"
    local timeout_secs="$3"
    local start_time
    start_time=$(date +%s)
    while true; do
        local val
        val=$(read_state "$key")
        if [ "$val" = "$expected" ]; then
            return 0
        fi
        local now
        now=$(date +%s)
        if [ $((now - start_time)) -ge "$timeout_secs" ]; then
            return 1
        fi
        sleep 0.05
    done
}

wait_for_sequence_greater() {
    local min_seq="$1"
    local timeout_secs="$2"
    local start_time
    start_time=$(date +%s)
    while true; do
        local cur_seq
        cur_seq=$(read_state "sequence")
        if [ -n "$cur_seq" ] && [ "$cur_seq" -gt "$min_seq" ] 2>/dev/null; then
            return 0
        fi
        local now
        now=$(date +%s)
        if [ $((now - start_time)) -ge "$timeout_secs" ]; then
            return 1
        fi
        sleep 0.05
    done
}

# --- Step 1: First-instance invalid page must exit non-zero deterministically ---
rm -f "$STATE_FILE"
set +e
FIRST_ERR=$(timeout 4s "$TARGET_BIN" --page invalid_dummy_page 2>&1)
FIRST_EXIT=$?
set -e

if [ "$FIRST_EXIT" -eq 124 ]; then
    echo "[FAIL] [REJECT: first_invalid_timeout] First-instance invalid page command timed out (hung)" >&2
    exit 1
fi
if [ "$FIRST_EXIT" -eq 0 ]; then
    echo "[FAIL] [REJECT: first_invalid_exit_code] First-instance invalid page command unexpectedly exited with 0" >&2
    exit 1
fi
if ! echo "$FIRST_ERR" | grep -q "Unknown settings page: invalid_dummy_page"; then
    echo "[FAIL] [REJECT: first_invalid_diagnostic] First-instance invalid page missing error diagnostic" >&2
    exit 1
fi
if [ -f "$STATE_FILE" ]; then
    echo "[FAIL] [REJECT: first_invalid_state_created] First-instance invalid page created state file unexpectedly" >&2
    exit 1
fi

# --- Step 2: Start primary instance with bounded handshake ---
rm -f "$STATE_FILE"
"$TARGET_BIN" --page overview &
SUITE_PRIMARY_PID=$!

primary_cleanup() {
    if [ -n "${SUITE_PRIMARY_PID:-}" ] && kill -0 "$SUITE_PRIMARY_PID" 2>/dev/null; then
        kill "$SUITE_PRIMARY_PID" 2>/dev/null || true
        wait "$SUITE_PRIMARY_PID" 2>/dev/null || true
    fi
}
trap primary_cleanup EXIT

if ! wait_for_condition "ready" "True" 5; then
    echo "[FAIL] [REJECT: ready_handshake] Primary instance failed ready handshake within 5s" >&2
    exit 1
fi

REP_PID=$(read_state "pid")
PAGE=$(read_state "visible_page")
WINS=$(read_state "window_count")

if [ "$REP_PID" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] [REJECT: ready_pid_mismatch] State PID ($REP_PID) does not match primary PID ($SUITE_PRIMARY_PID)" >&2
    exit 1
fi
if [ "$PAGE" != "overview" ]; then
    echo "[FAIL] [REJECT: initial_page_mismatch] Expected visible_page 'overview', got '$PAGE'" >&2
    exit 1
fi
if [ "$WINS" != "1" ]; then
    echo "[FAIL] [REJECT: initial_window_count] Expected window_count == 1, got '$WINS'" >&2
    exit 1
fi

# --- Step 3: Secondary instance navigates with sub-target focus ---
PREV_SEQ=$(read_state "sequence")
[ -z "$PREV_SEQ" ] && PREV_SEQ=0

set +e
SEC1_OUT=$(timeout 5s "$TARGET_BIN" --page candidates --target candidate-font-size 2>&1)
SEC1_EXIT=$?
set -e
if [ "$SEC1_EXIT" -ne 0 ]; then
    echo "[FAIL] [REJECT: secondary_navigation_failed] Secondary navigation command failed: $SEC1_OUT" >&2
    exit 1
fi

if ! wait_for_condition "visible_page" "candidates" 3; then
    echo "[FAIL] [REJECT: no_navigation] Failed to navigate to candidates (visible_page remained '$(read_state visible_page)')" >&2
    exit 1
fi

TARGET=$(read_state "focused_target")
if [ "$TARGET" != "candidate-font-size" ]; then
    echo "[FAIL] [REJECT: dropped_target] Expected focused_target 'candidate-font-size', got '$TARGET'" >&2
    exit 1
fi

WINS=$(read_state "window_count")
if [ "$WINS" != "1" ]; then
    echo "[FAIL] [REJECT: multiple_windows] Window count changed after secondary navigation (window_count=$WINS, expected 1)" >&2
    exit 1
fi
if [ "$(read_state "pid")" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] [REJECT: pid_changed] Primary PID changed after secondary navigation" >&2
    exit 1
fi

# --- Step 4: Secondary instance navigates via alias ---
set +e
SEC2_OUT=$(timeout 5s "$TARGET_BIN" --clipboard 2>&1)
SEC2_EXIT=$?
set -e
if [ "$SEC2_EXIT" -ne 0 ]; then
    echo "[FAIL] [REJECT: secondary_alias_failed] Secondary alias command failed: $SEC2_OUT" >&2
    exit 1
fi

if ! wait_for_condition "visible_page" "clipboard" 3; then
    echo "[FAIL] [REJECT: alias_navigation] Primary instance failed to navigate to clipboard via alias" >&2
    exit 1
fi
if [ "$(read_state "window_count")" != "1" ]; then
    echo "[FAIL] [REJECT: multiple_windows] Window count changed after alias navigation" >&2
    exit 1
fi
if [ "$(read_state "pid")" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] [REJECT: pid_changed] Primary PID changed after alias navigation" >&2
    exit 1
fi

# --- Step 5: Secondary instance invalid page rejection & fresh UI state verification ---
PRE_ERR_SEQ=$(read_state "sequence")
[ -z "$PRE_ERR_SEQ" ] && PRE_ERR_SEQ=0

set +e
SEC_ERR_OUT=$(timeout 5s "$TARGET_BIN" --page invalid_dummy_page 2>&1)
SEC_ERR_EXIT=$?
set -e
if [ "$SEC_ERR_EXIT" -eq 0 ]; then
    echo "[FAIL] [REJECT: secondary_invalid_exit_code] Secondary invalid page command unexpectedly succeeded (exit 0)" >&2
    exit 1
fi
if ! echo "$SEC_ERR_OUT" | grep -q "Unknown settings page: invalid_dummy_page"; then
    echo "[FAIL] [REJECT: secondary_invalid_diagnostic] Secondary invalid page missing error diagnostic" >&2
    exit 1
fi

if ! kill -0 "$SUITE_PRIMARY_PID" 2>/dev/null; then
    echo "[FAIL] [REJECT: primary_terminated] Primary instance terminated after secondary invalid page request" >&2
    exit 1
fi

# S1 requirement: ensure we observe the FRESH post-request UI state of the existing instance
CUR_SEQ=$(read_state "sequence")
[ -z "$CUR_SEQ" ] && CUR_SEQ=0

if [ "$CUR_SEQ" -le "$PRE_ERR_SEQ" ] || [ "$CUR_SEQ" -eq 0 ]; then
    # If the binary did not export a higher sequence during error handling (or lacks sequence),
    # remove the old state file and trigger an unnavigated sample probe to guarantee a fresh observation.
    rm -f "$STATE_FILE"
    timeout 5s "$TARGET_BIN" >/dev/null 2>&1 || true
    if ! wait_for_condition "ready" "True" 3; then
        echo "[FAIL] [REJECT: stale_observation] Failed to obtain fresh observation sequence after invalid request" >&2
        exit 1
    fi
fi

# Assert that the fresh GTK stack visible_page and window_count did NOT change
CURR_PAGE=$(read_state "visible_page")
if [ "$CURR_PAGE" != "clipboard" ]; then
    echo "[FAIL] [REJECT: invalid_page_side_effect] Primary visible page changed after secondary invalid request: expected 'clipboard', got '$CURR_PAGE'" >&2
    exit 1
fi
if [ "$(read_state "window_count")" != "1" ]; then
    echo "[FAIL] [REJECT: multiple_windows] Primary window count changed after secondary invalid request" >&2
    exit 1
fi
if [ "$(read_state "pid")" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] [REJECT: pid_changed] Primary PID changed after secondary invalid request" >&2
    exit 1
fi

echo "[PASS] All assertions in suite passed"
exit 0
SUITE_EOF
    chmod +x "$suite_script"

    dbus-run-session bash "$suite_script" "$TARGET_BIN" "$STATE_FILE"
}

# 5. Run positive test on the real production binary
echo "[INFO] Running navigation test suite against real binary: $SETTINGS_BIN"
run_suite "$SETTINGS_BIN"
echo "[PASS] Production binary passed all positive DBus navigation and single-instance tests."

# 6. Negative contrast verification (proof that gate rejects defective variants)
echo "[INFO] Running self-contained contrast verification to prove the gate rejects flawed implementations..."
CONTRAST_DIR="$SANDBOX_DIR/contrasts"
mkdir -p "$CONTRAST_DIR"

verify_contrast_rejected() {
    local fixture_bin="$1"
    local expected_reject="$2"
    local desc="$3"

    local output
    set +e
    output=$(run_suite "$fixture_bin" 2>&1)
    local status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        echo "[FAIL] Gate failed: contrast fixture '$desc' was accepted (expected rejection [$expected_reject])" >&2
        exit 1
    fi

    if echo "$output" | grep -q "\[REJECT: $expected_reject\]"; then
        echo "[PASS] Gate correctly rejected contrast '$desc' at expected stage [$expected_reject]."
    else
        echo "[FAIL] Contrast '$desc' failed, but not at expected stage [$expected_reject]! Output was:" >&2
        echo "$output" >&2
        exit 1
    fi
}

# 6.0: Self-contained Reviewer fake-settings
FAKE_SETTINGS_FIXTURE="$CONTRAST_DIR/fake_settings.py"
cat > "$FAKE_SETTINGS_FIXTURE" << 'EOF'
#!/usr/bin/env python3
import sys, time
if sys.argv[1:] == ['--page', 'overview']:
    time.sleep(25)
elif 'invalid_dummy_page' in sys.argv:
    print('Unknown settings page: invalid_dummy_page', file=sys.stderr)
sys.exit(0)
EOF
chmod +x "$FAKE_SETTINGS_FIXTURE"
verify_contrast_rejected "$FAKE_SETTINGS_FIXTURE" "first_invalid_exit_code" "fake-settings (no GTK, invalid exit 0)"

# Also verify reviewer's original fake-settings if present
if [ -x "/home/wsl/ModernIME/build/t18-review-evidence/fake-settings" ]; then
    verify_contrast_rejected "/home/wsl/ModernIME/build/t18-review-evidence/fake-settings" "first_invalid_exit_code" "reviewer original fake-settings"
fi

# 6.1: Contrast Variant: Invalid page exits 0 (非法页 exit 0)
CONTRAST_EXIT0="$CONTRAST_DIR/contrast_exit0.py"
cat > "$CONTRAST_EXIT0" << 'EOF'
#!/usr/bin/env python3
import sys, os, time, json

state_file = os.environ.get("MODERNIME_SETTINGS_STATE_FILE")

if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
    print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
    sys.exit(0) # Targeted flaw: exits 0 instead of 1

if "--page" in sys.argv and "overview" in sys.argv:
    if state_file:
        with open(state_file, "w") as f:
            json.dump({"pid": os.getpid(), "sequence": 1, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)
    time.sleep(20)
    sys.exit(0)

sys.exit(0)
EOF
chmod +x "$CONTRAST_EXIT0"
verify_contrast_rejected "$CONTRAST_EXIT0" "first_invalid_exit_code" "invalid page exit 0"

# 6.2: Contrast Variant: Does not navigate on secondary command (不导航)
CONTRAST_NO_NAV="$CONTRAST_DIR/contrast_no_nav.py"
cat > "$CONTRAST_NO_NAV" << 'EOF'
#!/usr/bin/env python3
import sys, os, time, json

state_file = os.environ.get("MODERNIME_SETTINGS_STATE_FILE")
pid_file = state_file + ".master_pid" if state_file else ""

if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
    print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
    sys.exit(1)

if "--page" in sys.argv and "overview" in sys.argv:
    master_pid = os.getpid()
    with open(pid_file, "w") as f:
        f.write(str(master_pid))
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": 1, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)
    time.sleep(20)
    sys.exit(0)

# Secondary command: reads master pid, keeps page 'overview' (flaw: no navigation)
if os.path.exists(pid_file):
    with open(pid_file) as f:
        master_pid = int(f.read().strip())
    cur_seq = 1
    if os.path.exists(state_file):
        try:
            data = json.load(open(state_file))
            cur_seq = data.get("sequence", 1) + 1
        except Exception:
            pass
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)

sys.exit(0)
EOF
chmod +x "$CONTRAST_NO_NAV"
verify_contrast_rejected "$CONTRAST_NO_NAV" "no_navigation" "does not navigate on secondary command"

# 6.3: Contrast Variant: Drops target (丢 target)
CONTRAST_NO_TARGET="$CONTRAST_DIR/contrast_no_target.py"
cat > "$CONTRAST_NO_TARGET" << 'EOF'
#!/usr/bin/env python3
import sys, os, time, json

state_file = os.environ.get("MODERNIME_SETTINGS_STATE_FILE")
pid_file = state_file + ".master_pid" if state_file else ""

if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
    print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
    sys.exit(1)

if "--page" in sys.argv and "overview" in sys.argv:
    master_pid = os.getpid()
    with open(pid_file, "w") as f:
        f.write(str(master_pid))
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": 1, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)
    time.sleep(20)
    sys.exit(0)

# Secondary command: navigates to candidates but drops target (focused_target: "")
if os.path.exists(pid_file):
    with open(pid_file) as f:
        master_pid = int(f.read().strip())
    cur_seq = 1
    if os.path.exists(state_file):
        try:
            cur_seq = json.load(open(state_file)).get("sequence", 1) + 1
        except Exception:
            pass
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 1, "visible_page": "candidates", "focused_target": "", "ready": True}, f)

sys.exit(0)
EOF
chmod +x "$CONTRAST_NO_TARGET"
verify_contrast_rejected "$CONTRAST_NO_TARGET" "dropped_target" "drops target focus"

# 6.4: Contrast Variant: Opens additional window (另开窗口)
CONTRAST_MULTI_WIN="$CONTRAST_DIR/contrast_multi_win.py"
cat > "$CONTRAST_MULTI_WIN" << 'EOF'
#!/usr/bin/env python3
import sys, os, time, json

state_file = os.environ.get("MODERNIME_SETTINGS_STATE_FILE")
pid_file = state_file + ".master_pid" if state_file else ""

if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
    print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
    sys.exit(1)

if "--page" in sys.argv and "overview" in sys.argv:
    master_pid = os.getpid()
    with open(pid_file, "w") as f:
        f.write(str(master_pid))
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": 1, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)
    time.sleep(20)
    sys.exit(0)

# Secondary command: window_count becomes 2
if os.path.exists(pid_file):
    with open(pid_file) as f:
        master_pid = int(f.read().strip())
    cur_seq = 1
    if os.path.exists(state_file):
        try:
            cur_seq = json.load(open(state_file)).get("sequence", 1) + 1
        except Exception:
            pass
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 2, "visible_page": "candidates", "focused_target": "candidate-font-size", "ready": True}, f)

sys.exit(0)
EOF
chmod +x "$CONTRAST_MULTI_WIN"
verify_contrast_rejected "$CONTRAST_MULTI_WIN" "multiple_windows" "opens multiple windows"

# 6.5: Contrast Variant: S1 Real UI Side Effect Mutant (existing instance mutates UI to candidates on invalid page)
CONTRAST_SIDE_EFFECT="$CONTRAST_DIR/contrast_side_effect.py"
cat > "$CONTRAST_SIDE_EFFECT" << 'EOF'
#!/usr/bin/env python3
import sys, os, time, json

state_file = os.environ.get("MODERNIME_SETTINGS_STATE_FILE")
pid_file = state_file + ".master_pid" if state_file else ""
page_file = state_file + ".cur_page" if state_file else ""

if "--page" in sys.argv and "overview" in sys.argv:
    master_pid = os.getpid()
    with open(pid_file, "w") as f:
        f.write(str(master_pid))
    with open(page_file, "w") as f:
        f.write("overview")
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": 1, "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": True}, f)
    time.sleep(20)
    sys.exit(0)

if not os.path.exists(pid_file):
    # First instance invalid page: exits 1 normally
    if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
        print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
        sys.exit(1)
    sys.exit(0)

with open(pid_file) as f:
    master_pid = int(f.read().strip())

cur_seq = 1
if os.path.exists(state_file):
    try:
        cur_seq = json.load(open(state_file)).get("sequence", 1) + 1
    except Exception:
        pass

# Secondary command: normal navigation
if "--page" in sys.argv and "candidates" in sys.argv:
    with open(page_file, "w") as f:
        f.write("candidates")
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 1, "visible_page": "candidates", "focused_target": "candidate-font-size", "ready": True}, f)
    sys.exit(0)

if "--clipboard" in sys.argv:
    with open(page_file, "w") as f:
        f.write("clipboard")
    with open(state_file, "w") as f:
        json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 1, "visible_page": "clipboard", "focused_target": "", "ready": True}, f)
    sys.exit(0)

# Secondary command: invalid page -> TARGET FLAW: changes UI to candidates, outputs error, exits 1
if "--page" in sys.argv and "invalid_dummy_page" in sys.argv:
    with open(page_file, "w") as f:
        f.write("candidates")
    print("Unknown settings page: invalid_dummy_page", file=sys.stderr)
    sys.exit(1)

# Sampling probe (no arguments): reports the actual page (now mutated to candidates!)
current_page = "overview"
if os.path.exists(page_file):
    with open(page_file) as f:
        current_page = f.read().strip()
with open(state_file, "w") as f:
    json.dump({"pid": master_pid, "sequence": cur_seq, "window_count": 1, "visible_page": current_page, "focused_target": "", "ready": True}, f)
sys.exit(0)
EOF
chmod +x "$CONTRAST_SIDE_EFFECT"
verify_contrast_rejected "$CONTRAST_SIDE_EFFECT" "invalid_page_side_effect" "S1 mutant (invalid request mutates UI to candidates)"

# Also test reviewer's compiled modernime-settings-mutant if present
REVIEWER_MUTANT="/home/wsl/ModernIME/build/t18-second-review-evidence/modernime-settings-mutant"
if [ -x "$REVIEWER_MUTANT" ]; then
    verify_contrast_rejected "$REVIEWER_MUTANT" "invalid_page_side_effect" "reviewer compiled modernime-settings-mutant binary"
fi

echo "[PASS] All negative contrast verification checks passed successfully."
echo "[PASS] T18 DBus navigation and single-instance gate validation complete."
