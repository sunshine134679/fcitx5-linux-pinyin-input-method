#!/usr/bin/env bash
# ModernIME T18: Isolated DBus Navigation & Target Activation Test
set -euo pipefail

# 1. Headless / Display Detection (strict compliance with AGENTS.md & gate rules)
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "[SKIP] No graphical display available for GTK DBus navigation test (exit 77)"
    exit 77
fi

# 2. Check for required binaries
SETTINGS_BIN="${SETTINGS_BIN:-${1:-}}"
if [ -z "$SETTINGS_BIN" ] || [ ! -x "$SETTINGS_BIN" ]; then
    # Try common build locations
    for candidate in \
        "build/parallel-t18-debug/tools/settings/modernime-settings" \
        "build/parallel-debug/tools/settings/modernime-settings" \
        "build/architecture-debug/tools/settings/modernime-settings" \
        "build/fcitx5-debug/tools/settings/modernime-settings" \
        "build/tools/settings/modernime-settings"; do
        if [ -x "$candidate" ]; then
            SETTINGS_BIN="$candidate"
            break
        fi
    done
fi

if [ ! -x "$SETTINGS_BIN" ]; then
    echo "[SKIP] modernime-settings binary not found or not executable at $SETTINGS_BIN (exit 77)"
    exit 77
fi

# 3. Create isolated sandbox environment
SANDBOX_DIR=$(mktemp -d "/tmp/modernime_dbus_t18_XXXXXX")
cleanup() {
    if [ -n "${PRIMARY_PID:-}" ] && kill -0 "$PRIMARY_PID" 2>/dev/null; then
        kill "$PRIMARY_PID" 2>/dev/null || true
        wait "$PRIMARY_PID" 2>/dev/null || true
    fi
    if [ -d "${SANDBOX_DIR:-}" ]; then
        fusermount -u "$SANDBOX_DIR/runtime/doc" 2>/dev/null || true
        fusermount -u "$SANDBOX_DIR/runtime/gvfs" 2>/dev/null || true
        fusermount -uz "$SANDBOX_DIR/runtime/doc" 2>/dev/null || true
        fusermount -uz "$SANDBOX_DIR/runtime/gvfs" 2>/dev/null || true
        rm -rf "$SANDBOX_DIR" 2>/dev/null || true
    fi
}
trap cleanup EXIT

export XDG_CONFIG_HOME="$SANDBOX_DIR/config"
export XDG_DATA_HOME="$SANDBOX_DIR/data"
export XDG_RUNTIME_DIR="$SANDBOX_DIR/runtime"
export HOME="$SANDBOX_DIR/home"
mkdir -p "$XDG_CONFIG_HOME/modernime" "$XDG_DATA_HOME/modernime" "$XDG_RUNTIME_DIR" "$HOME"
chmod 0700 "$XDG_RUNTIME_DIR"

# 4. Run isolated DBus session
echo "[INFO] Running isolated DBus navigation test using dbus-run-session..."
dbus-run-session bash -s -- "$SETTINGS_BIN" "$SANDBOX_DIR" << 'INNER_EOF'
set -euo pipefail

SETTINGS_BIN="$1"
SANDBOX_DIR="$2"

# Start primary instance in background
"$SETTINGS_BIN" --page overview &
PRIMARY_PID=$!

# Wait for primary instance to register on bus
sleep 1
if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
    echo "[ERROR] Primary settings instance failed to start" >&2
    exit 1
fi

# Dispatch second command: navigate existing instance to candidates with target
echo "[INFO] Testing secondary instance command: --page candidates --target candidate-font-size"
"$SETTINGS_BIN" --page candidates --target candidate-font-size

# Dispatch third command: navigate existing instance to clipboard via alias
echo "[INFO] Testing secondary instance command: --clipboard"
"$SETTINGS_BIN" --clipboard

# Dispatch fourth command: negative test with unknown page
echo "[INFO] Testing secondary instance negative command: --page invalid_dummy_page"
ERR_OUTPUT=$("$SETTINGS_BIN" --page invalid_dummy_page 2>&1 || true)
if echo "$ERR_OUTPUT" | grep -q "Unknown settings page: invalid_dummy_page"; then
    echo "[PASS] Successfully caught unknown page diagnosis: $ERR_OUTPUT"
else
    echo "[FAIL] Expected unknown page diagnostic not observed. Got: $ERR_OUTPUT" >&2
    exit 1
fi

# Cleanly stop primary instance
kill "$PRIMARY_PID" 2>/dev/null || true
wait "$PRIMARY_PID" 2>/dev/null || true

echo "[PASS] All isolated DBus secondary-instance navigation tests passed!"
INNER_EOF
