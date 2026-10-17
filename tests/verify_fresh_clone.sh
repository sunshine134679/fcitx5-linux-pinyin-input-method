#!/usr/bin/env bash
set -euo pipefail

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/modernime-fresh-clone-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

printf '=== 1. 克隆代码仓库到沙箱隔离目录 ===\n'
# Build a local snapshot first: cloning HEAD alone silently validates the old
# version when this script is run before committing a fix. The original index,
# branches and user files remain untouched.
git clone --no-hardlinks "$project_root" "$test_root/snapshot"
git -C "$project_root" diff --binary HEAD > "$test_root/working-tree.patch"
if [[ -s "$test_root/working-tree.patch" ]]; then
    git -C "$test_root/snapshot" apply "$test_root/working-tree.patch"
fi
while IFS= read -r -d '' path; do
    mkdir -p -- "$test_root/snapshot/$(dirname -- "$path")"
    cp -a -- "$project_root/$path" "$test_root/snapshot/$path"
done < <(git -C "$project_root" ls-files --others --exclude-standard -z)
git -C "$test_root/snapshot" add -A
if ! git -C "$test_root/snapshot" diff --cached --quiet; then
    git -C "$test_root/snapshot" -c user.name='ModernIME Verification' \
        -c user.email='verification@localhost' -c commit.gpgsign=false \
        -c core.hooksPath=/dev/null commit -m 'Isolated verification snapshot' >/dev/null
fi
git clone --no-hardlinks "$test_root/snapshot" "$test_root/repo"

mkdir -p "$test_root/home"
export HOME="$test_root/home"
export XDG_DATA_HOME="$test_root/data"
export XDG_CONFIG_HOME="$test_root/config"
export XDG_CACHE_HOME="$test_root/cache"
export MODERNIME_PREFIX="$test_root/prefix"
export MODERNIME_CONFIG_HOME="$test_root/config"
export MODERNIME_DESKTOP_DIR="$test_root/Desktop"
export MODERNIME_SKIP_FCITX_RESTART=1

printf '=== 2. 在沙箱环境中执行 install.sh ===\n'
(cd "$test_root/repo" && ./install.sh)

printf '=== 3. 验证沙箱安装产物完整性 ===\n'
test -f "$MODERNIME_PREFIX/bin/modernime-settings"
test -x "$MODERNIME_PREFIX/bin/modernime-settings"
installed_version=$(env -u DISPLAY -u WAYLAND_DISPLAY "$MODERNIME_PREFIX/bin/modernime-settings" --version)
[[ "$installed_version" == *"ModernIME settings"* ]]
# Verify internal capture collector handles missing args by exiting with code 1
set +e
env -u DISPLAY -u WAYLAND_DISPLAY "$MODERNIME_PREFIX/bin/modernime-settings" --internal-capture-collector >/dev/null 2>&1
t4_missing_status=$?
set -e
if [[ "$t4_missing_status" -ne 1 ]]; then
    echo "ERROR: Expected exit code 1 when internal capture collector is invoked without args, got $t4_missing_status" >&2
    exit 1
fi

# Execute a synthetic task with installed binary in headless environment and verify full result
python3 - "$MODERNIME_PREFIX" "${test_root:-/tmp}" << 'PYEOF'
import os
import sys
import json
import time
import signal
import select
import shutil
import tempfile
import subprocess
from pathlib import Path

prefix_arg = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else os.environ.get("MODERNIME_PREFIX")
if not prefix_arg:
    sys.exit("ERROR: MODERNIME_PREFIX is not set")
prefix = Path(prefix_arg)

test_root_arg = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else os.environ.get("test_root", "/tmp")
test_root = Path(test_root_arg)

binary = prefix / "bin" / "modernime-settings"
if not binary.is_file() or not os.access(binary, os.X_OK):
    sys.exit(f"ERROR: Settings binary not found or not executable: {binary}")

# Create isolated operation directory with 0700 permissions
op_dir = Path(tempfile.mkdtemp(prefix="modernime-install-collector-", dir=str(test_root)))
os.chmod(op_dir, 0o700)

spec_path = op_dir / "spec.dat"
spec_content = (
    "operationId=98765\n"
    "token=install-token-98765\n"
    "timeoutMs=5000\n"
    "mode=1\n"
    "perStreamBufferBytes=1048576\n"
    "perStreamResultBytes=65536\n"
    "perStreamFileBytes=1048576\n"
    "argc=3\n"
    "argv=/usr/bin/python3\n"
    "argv=-c\n"
    "argv=import sys; sys.stdout.write('INSTALL_COLLECTOR_STDOUT_OK\\n'); sys.stderr.write('INSTALL_COLLECTOR_STDERR_OK\\n')\n"
    "envc=0\n"
)
spec_path.write_text(spec_content, encoding="utf-8")
os.chmod(spec_path, 0o600)

fifo_path = op_dir / "handshake.fifo"
os.mkfifo(fifo_path, 0o600)
# Open FIFO read end non-blocking with O_RDWR so opening write end will not block or race
fifo_fd = os.open(fifo_path, os.O_RDWR | os.O_NONBLOCK)

env = os.environ.copy()
env.pop("DISPLAY", None)
env.pop("WAYLAND_DISPLAY", None)

start_time = time.monotonic()
total_timeout = 5.0
deadline = start_time + total_timeout

proc = None
clean_exit = False
fifo_closed = False

try:
    # Spawn collector in a new process group so all descendants belong to proc.pid group
    proc = subprocess.Popen(
        [
            str(binary),
            "--internal-capture-collector",
            "--op-dir", str(op_dir),
            "--handshake-fd", str(fifo_fd)
        ],
        env=env,
        start_new_session=True,
        pass_fds=[fifo_fd]
    )

    # 1. Read handshake byte with monotonic deadline
    poll = select.poll()
    poll.register(fifo_fd, select.POLLIN | select.POLLHUP | select.POLLERR)
    hs_byte = None

    while time.monotonic() < deadline:
        rem_ms = int(max(0.0, min(deadline - time.monotonic(), 4.0)) * 1000)
        events = poll.poll(max(1, min(50, rem_ms)))
        if events:
            for fd, ev in events:
                if ev & select.POLLIN:
                    try:
                        chunk = os.read(fifo_fd, 1)
                        if chunk:
                            hs_byte = chunk
                            break
                    except OSError:
                        pass
                if ev & (select.POLLHUP | select.POLLERR):
                    break
        if hs_byte is not None:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.02)

    os.close(fifo_fd)
    fifo_closed = True

    if hs_byte not in (b"R", b"1"):
        sys.exit(f"ERROR: Handshake failed (got {hs_byte!r}, process exited={proc.poll()})")

    # 2. Wait for collector to exit with overall monotonic deadline
    exited = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            exited = True
            break
        time.sleep(0.02)

    if not exited:
        sys.exit(f"ERROR: Collector process stalled after handshake (exceeded total monotonic timeout of {total_timeout}s)")

    if proc.returncode != 0:
        sys.exit(f"ERROR: Collector process exited with non-zero code {proc.returncode}")

    # 3. Comprehensive verification of outputs and state.json (U2-B)
    state_file = op_dir / "state.json"
    stdout_file = op_dir / "stdout.tail"
    stderr_file = op_dir / "stderr.tail"

    if not state_file.is_file():
        sys.exit("ERROR: state.json is missing")
    if not stdout_file.is_file():
        sys.exit("ERROR: stdout.tail is missing")
    if not stderr_file.is_file():
        sys.exit("ERROR: stderr.tail is missing")

    # Permissions check (directory 0700, files 0600)
    if (op_dir.stat().st_mode & 0o777) != 0o700:
        sys.exit(f"ERROR: op_dir permissions invalid: {oct(op_dir.stat().st_mode)}")
    if (spec_path.stat().st_mode & 0o777) != 0o600:
        sys.exit(f"ERROR: spec.dat permissions invalid: {oct(spec_path.stat().st_mode)}")
    if (state_file.stat().st_mode & 0o777) != 0o600:
        sys.exit(f"ERROR: state.json permissions invalid: {oct(state_file.stat().st_mode)}")
    if (stdout_file.stat().st_mode & 0o777) != 0o600:
        sys.exit(f"ERROR: stdout.tail permissions invalid: {oct(stdout_file.stat().st_mode)}")
    if (stderr_file.stat().st_mode & 0o777) != 0o600:
        sys.exit(f"ERROR: stderr.tail permissions invalid: {oct(stderr_file.stat().st_mode)}")

    # Resource budget checks
    state_sz = state_file.stat().st_size
    if state_sz <= 0 or state_sz > 4096:
        sys.exit(f"ERROR: state.json size {state_sz} out of bounds (1..4096)")

    stdout_sz = stdout_file.stat().st_size
    if stdout_sz > 65536:
        sys.exit(f"ERROR: stdout.tail size {stdout_sz} exceeds result limit 65536")

    stderr_sz = stderr_file.stat().st_size
    if stderr_sz > 65536:
        sys.exit(f"ERROR: stderr.tail size {stderr_sz} exceeds result limit 65536")

    stdout_diag = op_dir / "stdout.diag"
    if stdout_diag.exists() and stdout_diag.stat().st_size > 1048576:
        sys.exit(f"ERROR: stdout.diag size {stdout_diag.stat().st_size} exceeds file limit 1048576")

    stderr_diag = op_dir / "stderr.diag"
    if stderr_diag.exists() and stderr_diag.stat().st_size > 1048576:
        sys.exit(f"ERROR: stderr.diag size {stderr_diag.stat().st_size} exceeds file limit 1048576")

    # Strict JSON parsing rejecting duplicate keys
    def reject_duplicate_keys(ordered_pairs):
        d = {}
        for k, v in ordered_pairs:
            if k in d:
                raise ValueError(f"Duplicate key detected in state.json: {k}")
            d[k] = v
        return d

    raw_json = state_file.read_text(encoding="utf-8")
    try:
        data = json.loads(raw_json, object_pairs_hook=reject_duplicate_keys)
    except Exception as e:
        sys.exit(f"ERROR: state.json parse failure: {e}")

    if not isinstance(data, dict):
        sys.exit("ERROR: state.json root is not an object")

    # Exact field type and value assertions
    if type(data.get("operationId")) is not int or data.get("operationId") != 98765:
        sys.exit(f"ERROR: operationId mismatch: {data.get('operationId')!r}")
    if type(data.get("token")) is not str or data.get("token") != "install-token-98765":
        sys.exit(f"ERROR: token mismatch: {data.get('token')!r}")
    if type(data.get("state")) is not str or data.get("state") != "Exited":
        sys.exit(f"ERROR: state mismatch: {data.get('state')!r}")
    if type(data.get("exitStatus")) is not int or data.get("exitStatus") != 0:
        sys.exit(f"ERROR: exitStatus mismatch: {data.get('exitStatus')!r}")
    if type(data.get("collectorPid")) is not int or data.get("collectorPid") <= 0:
        sys.exit(f"ERROR: collectorPid invalid: {data.get('collectorPid')!r}")
    if type(data.get("stdoutBytes")) is not int or data.get("stdoutBytes") < 0:
        sys.exit(f"ERROR: stdoutBytes invalid: {data.get('stdoutBytes')!r}")
    if type(data.get("stderrBytes")) is not int or data.get("stderrBytes") < 0:
        sys.exit(f"ERROR: stderrBytes invalid: {data.get('stderrBytes')!r}")
    if type(data.get("bufferPeak")) is not int or data.get("bufferPeak") < 0:
        sys.exit(f"ERROR: bufferPeak invalid: {data.get('bufferPeak')!r}")
    if type(data.get("filePeak")) is not int or data.get("filePeak") < 0:
        sys.exit(f"ERROR: filePeak invalid: {data.get('filePeak')!r}")
    if type(data.get("stdoutTruncated")) is not bool:
        sys.exit(f"ERROR: stdoutTruncated not bool: {data.get('stdoutTruncated')!r}")
    if type(data.get("stderrTruncated")) is not bool:
        sys.exit(f"ERROR: stderrTruncated not bool: {data.get('stderrTruncated')!r}")

    # Output markers check
    stdout_text = stdout_file.read_text(encoding="utf-8", errors="replace")
    stderr_text = stderr_file.read_text(encoding="utf-8", errors="replace")
    if "INSTALL_COLLECTOR_STDOUT_OK" not in stdout_text:
        sys.exit("ERROR: INSTALL_COLLECTOR_STDOUT_OK missing from stdout.tail")
    if "INSTALL_COLLECTOR_STDERR_OK" not in stderr_text:
        sys.exit("ERROR: INSTALL_COLLECTOR_STDERR_OK missing from stderr.tail")

    clean_exit = True

finally:
    if not fifo_closed:
        try:
            os.close(fifo_fd)
        except OSError:
            pass

    # Terminate the process group to ensure collector and any spawned descendants are killed
    if proc is not None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except Exception:
            pass
        try:
            proc.wait(timeout=0.2)
        except Exception:
            pass

    # Clean up operation directory on successful exit
    if clean_exit:
        try:
            shutil.rmtree(op_dir, ignore_errors=True)
        except Exception:
            pass
PYEOF
test -f "$MODERNIME_PREFIX/bin/modernime-session-init"
test -f "$MODERNIME_PREFIX/share/modernime/install_state.sh"
test -x "$MODERNIME_PREFIX/bin/modernime-session-init"
{ test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so" || \
  test -f "$MODERNIME_PREFIX/lib64/fcitx5/modernime_fcitx5.so" || \
  test -f "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"; }
{ test -f "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so" || \
  test -f "$MODERNIME_PREFIX/lib64/fcitx5/modernime_ui.so" || \
  test -f "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"; }
test -f "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
test -f "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test -f "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
grep -Fx 'Name=modernime' "$MODERNIME_CONFIG_HOME/fcitx5/profile" >/dev/null
test -x "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
for icon_size in 16 32 48 64 128 256 512; do
    test -s "$MODERNIME_PREFIX/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
done
grep -Fx "Icon=$MODERNIME_PREFIX/share/icons/hicolor/512x512/apps/modernime.png" "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop" >/dev/null

# Validate actual Fcitx metadata discovery using the installed session environment.
set -a
source "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"
set +a
"$test_root/repo/build/install-debug/tests/modernime_install_discovery_tests" "$MODERNIME_PREFIX"
test ! -e "$HOME/.xprofile"

printf '=== 4. 在沙箱环境中执行 uninstall.sh ===\n'
(cd "$test_root/repo" && ./uninstall.sh)

printf '=== 5. 验证沙箱卸载清理完全性 ===\n'
test ! -e "$MODERNIME_PREFIX/bin/modernime-settings"
test ! -e "$MODERNIME_PREFIX/bin/modernime-session-init"
test ! -e "$MODERNIME_PREFIX/share/modernime/install_state.sh"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/LICENSE"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/copyright"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/THIRD_PARTY_NOTICES.md"
test ! -e "$MODERNIME_PREFIX/share/doc/modernime/SOURCES.json"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib64/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_fcitx5.so"
test ! -e "$MODERNIME_PREFIX/lib/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_PREFIX/lib64/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_PREFIX/lib/x86_64-linux-gnu/fcitx5/modernime_ui.so"
test ! -e "$MODERNIME_DESKTOP_DIR/modernime-settings.desktop"
for icon_size in 16 32 48 64 128 256 512; do
    test ! -e "$MODERNIME_PREFIX/share/icons/hicolor/${icon_size}x${icon_size}/apps/modernime.png"
done
test ! -e "$MODERNIME_CONFIG_HOME/autostart/modernime-fcitx5-session.desktop"
test ! -e "$MODERNIME_CONFIG_HOME/environment.d/90-modernime.conf"

printf '=== 6. 沙箱全新克隆与全生命周期安装卸载验证 100%% 通过！ ===\n'
