#!/usr/bin/env bash
# Runtime Controller Concurrency & Collision Isolation Test
# Verifies that multiple concurrent executions of runtime_controller_tests
# sharing the same parent TMPDIR execute safely without directory collision.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${1:-${BUILD_DIR:-$REPO_ROOT/build/architecture-final-debug}}"

if [[ ! -d "$BUILD_DIR" ]]; then
    # Fallback to standard build directory if present
    BUILD_DIR="$REPO_ROOT/build"
fi

RUNTIME_BIN="$BUILD_DIR/tests/modernime_runtime_controller_tests"
REMOTE_BIN="$BUILD_DIR/tests/modernime_fake_fcitx5_remote"
FCITX_BIN="$BUILD_DIR/tests/modernime_fake_fcitx5"

if [[ ! -x "$RUNTIME_BIN" || ! -x "$REMOTE_BIN" || ! -x "$FCITX_BIN" ]]; then
    echo "ERROR: Required test binaries not found in $BUILD_DIR/tests"
    exit 1
fi

SHARED_TMPDIR=$(mktemp -d /tmp/modernime-shared-parent-tmp.XXXXXX)
trap 'rm -rf "$SHARED_TMPDIR"' EXIT

echo "=== Running D2 Concurrency Test: 2 Instances Sharing Parent TMPDIR ($SHARED_TMPDIR) ==="

LOG1="$SHARED_TMPDIR/inst1.log"
LOG2="$SHARED_TMPDIR/inst2.log"

export TMPDIR="$SHARED_TMPDIR"

# Launch two instances concurrently
"$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG1" 2>&1 &
PID1=$!

"$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG2" 2>&1 &
PID2=$!

EXIT1=0
EXIT2=0
wait "$PID1" || EXIT1=$?
wait "$PID2" || EXIT2=$?

if [[ $EXIT1 -ne 0 || $EXIT2 -ne 0 ]]; then
    echo "[FAIL] Concurrent instances collided! Exit codes: inst1=$EXIT1, inst2=$EXIT2"
    echo "--- Instance 1 Log ---"
    cat "$LOG1"
    echo "--- Instance 2 Log ---"
    cat "$LOG2"
    exit 1
fi

echo "[PASS] Both concurrent instances succeeded (exit 0) without collision!"

echo "=== Running D2 Concurrency Test: runtime_controller_tests + configuration_contract_test.sh ==="
A07_SCRIPT="$REPO_ROOT/tests/configuration_contract_test.sh"
if [[ -f "$A07_SCRIPT" ]]; then
    LOG_A07="$SHARED_TMPDIR/a07.log"
    LOG_RUNTIME="$SHARED_TMPDIR/runtime_a07.log"

    bash "$A07_SCRIPT" --build-dir "$BUILD_DIR" > "$LOG_A07" 2>&1 &
    PID_A07=$!

    "$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG_RUNTIME" 2>&1 &
    PID_RUNTIME=$!

    EXIT_A07=0
    EXIT_RUNTIME=0
    wait "$PID_A07" || EXIT_A07=$?
    wait "$PID_RUNTIME" || EXIT_RUNTIME=$?

    if [[ $EXIT_A07 -ne 0 || $EXIT_RUNTIME -ne 0 ]]; then
        echo "[FAIL] Concurrent A07 and runtime test collided! Exit codes: a07=$EXIT_A07, runtime=$EXIT_RUNTIME"
        cat "$LOG_A07"
        cat "$LOG_RUNTIME"
        exit 1
    fi
    echo "[PASS] Concurrent A07 and runtime_controller_tests succeeded without collision!"
fi

echo "[ALL PASS] D2 Runtime Controller Isolation verified successfully."
