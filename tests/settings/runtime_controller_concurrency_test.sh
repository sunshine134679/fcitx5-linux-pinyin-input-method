#!/usr/bin/env bash
# Runtime Controller Concurrency & Collision Isolation Test
# Verifies that multiple concurrent executions of runtime_controller_tests
# sharing the same parent TMPDIR execute safely without directory collision.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${1:-${BUILD_DIR:-}}"
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
    echo "ERROR: BUILD_DIR='$BUILD_DIR' is missing or not a directory" >&2
    exit 1
fi

RUNTIME_BIN="$BUILD_DIR/tests/modernime_runtime_controller_tests"
REMOTE_BIN="$BUILD_DIR/tests/modernime_fake_fcitx5_remote"
FCITX_BIN="$BUILD_DIR/tests/modernime_fake_fcitx5"

if [[ ! -x "$RUNTIME_BIN" || ! -x "$REMOTE_BIN" || ! -x "$FCITX_BIN" ]]; then
    echo "ERROR: Required test binaries not found in $BUILD_DIR/tests" >&2
    exit 1
fi

SHARED_TMPDIR=$(mktemp -d /tmp/modernime-shared-parent-tmp.XXXXXX)
trap 'rm -rf "$SHARED_TMPDIR"' EXIT

echo "=== Running D2 Concurrency Test: 3 Concurrent Instances Sharing Parent TMPDIR ($SHARED_TMPDIR) ==="

LOG1="$SHARED_TMPDIR/inst1.log"
LOG2="$SHARED_TMPDIR/inst2.log"
LOG3="$SHARED_TMPDIR/inst3.log"

export TMPDIR="$SHARED_TMPDIR"

# Launch 3 instances concurrently
"$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG1" 2>&1 &
PID1=$!

"$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG2" 2>&1 &
PID2=$!

"$RUNTIME_BIN" "$REMOTE_BIN" "$FCITX_BIN" > "$LOG3" 2>&1 &
PID3=$!

EXIT1=0
EXIT2=0
EXIT3=0
wait "$PID1" || EXIT1=$?
wait "$PID2" || EXIT2=$?
wait "$PID3" || EXIT3=$?

if [[ $EXIT1 -ne 0 || $EXIT2 -ne 0 || $EXIT3 -ne 0 ]]; then
    echo "[FAIL] Concurrent instances collided! Exit codes: inst1=$EXIT1, inst2=$EXIT2, inst3=$EXIT3" >&2
    echo "--- Instance 1 Log ---" >&2
    cat "$LOG1" >&2
    echo "--- Instance 2 Log ---" >&2
    cat "$LOG2" >&2
    echo "--- Instance 3 Log ---" >&2
    cat "$LOG3" >&2
    exit 1
fi

echo "[PASS] All 3 concurrent instances succeeded (exit 0) without directory collision!"
echo "[ALL PASS] D2 Runtime Controller Isolation verified successfully."
