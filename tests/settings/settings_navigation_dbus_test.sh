#!/usr/bin/env bash
# ModernIME T18: Isolated DBus Navigation & Target Activation Test
set -euo pipefail

# 1. Headless / Display Detection (strict compliance with AGENTS.md & gate rules)
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "[SKIP] No graphical display available for GTK DBus navigation test (exit 77)"
    exit 77
fi

# 2. Check for required binary (explicit requirement: must exist and be executable, otherwise exit 1)
SETTINGS_BIN="${1:-${SETTINGS_BIN:-}}"
if [ -z "$SETTINGS_BIN" ]; then
    echo "[ERROR] Missing required settings binary argument (exit 1)" >&2
    exit 1
fi

if [ ! -x "$SETTINGS_BIN" ]; then
    echo "[ERROR] Required binary '$SETTINGS_BIN' does not exist or is not executable (exit 1)" >&2
    exit 1
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
    local SUB_SANDBOX="$SANDBOX_DIR/suite_run_$$"
    rm -rf "$SUB_SANDBOX"
    mkdir -p "$SUB_SANDBOX"
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

# --- Step 1: First-instance invalid page must exit non-zero deterministically ---
rm -f "$STATE_FILE"
set +e
FIRST_ERR=$(timeout 4s "$TARGET_BIN" --page invalid_dummy_page 2>&1)
FIRST_EXIT=$?
set -e

if [ "$FIRST_EXIT" -eq 124 ]; then
    echo "[FAIL] First-instance invalid page command timed out (hung)" >&2
    exit 1
fi
if [ "$FIRST_EXIT" -eq 0 ]; then
    echo "[FAIL] First-instance invalid page command unexpectedly exited with 0" >&2
    exit 1
fi
if ! echo "$FIRST_ERR" | grep -q "Unknown settings page: invalid_dummy_page"; then
    echo "[FAIL] First-instance invalid page missing error diagnostic" >&2
    exit 1
fi
if [ -f "$STATE_FILE" ]; then
    echo "[FAIL] First-instance invalid page created state file unexpectedly" >&2
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
    echo "[FAIL] Primary instance failed ready handshake within 5s" >&2
    exit 1
fi

REP_PID=$(read_state "pid")
PAGE=$(read_state "visible_page")
WINS=$(read_state "window_count")

if [ "$REP_PID" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] State PID ($REP_PID) does not match primary PID ($SUITE_PRIMARY_PID)" >&2
    exit 1
fi
if [ "$PAGE" != "overview" ]; then
    echo "[FAIL] Expected visible_page 'overview', got '$PAGE'" >&2
    exit 1
fi
if [ "$WINS" != "1" ]; then
    echo "[FAIL] Expected window_count == 1, got '$WINS'" >&2
    exit 1
fi

# --- Step 3: Secondary instance navigates with sub-target focus ---
set +e
SEC1_OUT=$("$TARGET_BIN" --page candidates --target candidate-font-size 2>&1)
SEC1_EXIT=$?
set -e
if [ "$SEC1_EXIT" -ne 0 ]; then
    echo "[FAIL] Secondary navigation command failed: $SEC1_OUT" >&2
    exit 1
fi

if ! wait_for_condition "visible_page" "candidates" 3; then
    echo "[FAIL] Primary instance failed to navigate to candidates" >&2
    exit 1
fi

TARGET=$(read_state "focused_target")
if [ "$TARGET" != "candidate-font-size" ]; then
    echo "[FAIL] Expected focused_target 'candidate-font-size', got '$TARGET'" >&2
    exit 1
fi

if [ "$(read_state "window_count")" != "1" ]; then
    echo "[FAIL] Window count changed after secondary navigation" >&2
    exit 1
fi
if [ "$(read_state "pid")" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] Primary PID changed after secondary navigation" >&2
    exit 1
fi

# --- Step 4: Secondary instance navigates via alias ---
set +e
SEC2_OUT=$("$TARGET_BIN" --clipboard 2>&1)
SEC2_EXIT=$?
set -e
if [ "$SEC2_EXIT" -ne 0 ]; then
    echo "[FAIL] Secondary alias command failed: $SEC2_OUT" >&2
    exit 1
fi

if ! wait_for_condition "visible_page" "clipboard" 3; then
    echo "[FAIL] Primary instance failed to navigate to clipboard via alias" >&2
    exit 1
fi
if [ "$(read_state "window_count")" != "1" ]; then
    echo "[FAIL] Window count changed after alias navigation" >&2
    exit 1
fi
if [ "$(read_state "pid")" != "$SUITE_PRIMARY_PID" ]; then
    echo "[FAIL] Primary PID changed after alias navigation" >&2
    exit 1
fi

# --- Step 5: Secondary instance invalid page rejection ---
set +e
SEC_ERR_OUT=$("$TARGET_BIN" --page invalid_dummy_page 2>&1)
SEC_ERR_EXIT=$?
set -e
if [ "$SEC_ERR_EXIT" -eq 0 ]; then
    echo "[FAIL] Secondary invalid page command unexpectedly succeeded (exit 0)" >&2
    exit 1
fi
if ! echo "$SEC_ERR_OUT" | grep -q "Unknown settings page: invalid_dummy_page"; then
    echo "[FAIL] Secondary invalid page missing error diagnostic" >&2
    exit 1
fi

if ! kill -0 "$SUITE_PRIMARY_PID" 2>/dev/null; then
    echo "[FAIL] Primary instance terminated after secondary invalid page request" >&2
    exit 1
fi

# Primary state must remain untouched
if [ "$(read_state "visible_page")" != "clipboard" ]; then
    echo "[FAIL] Primary visible page changed after secondary invalid request" >&2
    exit 1
fi
if [ "$(read_state "window_count")" != "1" ]; then
    echo "[FAIL] Primary window count changed after secondary invalid request" >&2
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
echo "[INFO] Running contrast verification to prove the gate rejects flawed implementations..."
CONTRAST_DIR="$SANDBOX_DIR/contrasts"
mkdir -p "$CONTRAST_DIR"

# 6.0: Reviewer's fake-settings
REVIEW_FAKE="/home/wsl/ModernIME/build/t18-review-evidence/fake-settings"
if [ -x "$REVIEW_FAKE" ]; then
    if run_suite "$REVIEW_FAKE" >/dev/null 2>&1; then
        echo "[FAIL] Gate failed: Reviewer fake-settings was accepted!" >&2
        exit 1
    fi
    echo "[PASS] Gate correctly rejected reviewer's fake-settings."
fi

# 6.1: Contrast Variant: Invalid page exits 0 (非法页 exit 0)
CONTRAST_EXIT0="$CONTRAST_DIR/contrast_exit0.sh"
cat > "$CONTRAST_EXIT0" << 'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--page" ] && [ "${2:-}" = "invalid_dummy_page" ]; then
    echo "Unknown settings page: invalid_dummy_page" >&2
    exit 0
fi
if [ -n "${MODERNIME_SETTINGS_STATE_FILE:-}" ]; then
    echo '{"pid": '$$', "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
fi
sleep 5
EOF
chmod +x "$CONTRAST_EXIT0"

if run_suite "$CONTRAST_EXIT0" >/dev/null 2>&1; then
    echo "[FAIL] Gate failed: contrast variant 'invalid page exit 0' was accepted!" >&2
    exit 1
fi
echo "[PASS] Gate correctly rejected contrast variant (invalid page exit 0)."

# 6.2: Contrast Variant: Does not navigate (不导航)
CONTRAST_NO_NAV="$CONTRAST_DIR/contrast_no_nav.sh"
cat > "$CONTRAST_NO_NAV" << 'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--page" ] && [ "${2:-}" = "invalid_dummy_page" ]; then
    echo "Unknown settings page: invalid_dummy_page" >&2
    exit 1
fi
if [ -n "${MODERNIME_SETTINGS_STATE_FILE:-}" ] && [ ! -f "$MODERNIME_SETTINGS_STATE_FILE" ]; then
    echo '{"pid": '$$', "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
fi
# Secondary commands do not update visible_page
sleep 5
EOF
chmod +x "$CONTRAST_NO_NAV"

if run_suite "$CONTRAST_NO_NAV" >/dev/null 2>&1; then
    echo "[FAIL] Gate failed: contrast variant 'no navigation' was accepted!" >&2
    exit 1
fi
echo "[PASS] Gate correctly rejected contrast variant (no navigation)."

# 6.3: Contrast Variant: Drops target (丢 target)
CONTRAST_NO_TARGET="$CONTRAST_DIR/contrast_no_target.sh"
cat > "$CONTRAST_NO_TARGET" << 'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--page" ] && [ "${2:-}" = "invalid_dummy_page" ]; then
    echo "Unknown settings page: invalid_dummy_page" >&2
    exit 1
fi
if [ -n "${MODERNIME_SETTINGS_STATE_FILE:-}" ]; then
    if [ ! -f "$MODERNIME_SETTINGS_STATE_FILE" ]; then
        echo '{"pid": '$$', "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
    else
        # Navigates to candidates but drops target
        echo '{"pid": '$$', "window_count": 1, "visible_page": "candidates", "focused_target": "", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
    fi
fi
sleep 5
EOF
chmod +x "$CONTRAST_NO_TARGET"

if run_suite "$CONTRAST_NO_TARGET" >/dev/null 2>&1; then
    echo "[FAIL] Gate failed: contrast variant 'dropped target' was accepted!" >&2
    exit 1
fi
echo "[PASS] Gate correctly rejected contrast variant (dropped target)."

# 6.4: Contrast Variant: Opens additional window (另开窗口)
CONTRAST_MULTI_WIN="$CONTRAST_DIR/contrast_multi_win.sh"
cat > "$CONTRAST_MULTI_WIN" << 'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = "--page" ] && [ "${2:-}" = "invalid_dummy_page" ]; then
    echo "Unknown settings page: invalid_dummy_page" >&2
    exit 1
fi
if [ -n "${MODERNIME_SETTINGS_STATE_FILE:-}" ]; then
    if [ ! -f "$MODERNIME_SETTINGS_STATE_FILE" ]; then
        echo '{"pid": '$$', "window_count": 1, "visible_page": "overview", "focused_target": "", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
    else
        # Navigates but opens second window
        echo '{"pid": '$$', "window_count": 2, "visible_page": "candidates", "focused_target": "candidate-font-size", "ready": true}' > "$MODERNIME_SETTINGS_STATE_FILE"
    fi
fi
sleep 5
EOF
chmod +x "$CONTRAST_MULTI_WIN"

if run_suite "$CONTRAST_MULTI_WIN" >/dev/null 2>&1; then
    echo "[FAIL] Gate failed: contrast variant 'multiple windows' was accepted!" >&2
    exit 1
fi
echo "[PASS] Gate correctly rejected contrast variant (multiple windows)."

echo "[PASS] All negative contrast verification checks passed successfully."
echo "[PASS] T18 DBus navigation and single-instance gate validation complete."
