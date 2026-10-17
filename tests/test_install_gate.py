#!/usr/bin/env python3
import os
import sys
import time
import json
import signal
import shutil
import tempfile
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

def extract_gate_block():
    script_path = REPO_ROOT / "tests" / "verify_fresh_clone.sh"
    content = script_path.read_text(encoding="utf-8")
    start_marker = "# Execute a synthetic task"
    end_marker = 'test -f "$MODERNIME_PREFIX/bin/modernime-session-init"'
    if start_marker not in content or end_marker not in content:
        raise RuntimeError("Failed to find markers in tests/verify_fresh_clone.sh")
    start = content.index(start_marker)
    end = content.index(end_marker)
    return content[start:end]

def run_gate_test(case_name, block, real_binary=None):
    with tempfile.TemporaryDirectory(prefix=f"modernime-gate-test-{case_name}-") as td:
        root = Path(td)
        prefix = root / "prefix"
        (prefix / "bin").mkdir(parents=True)
        binary = prefix / "bin" / "modernime-settings"
        descendant_pid_file = root / "descendant.pid"

        if case_name == "real_binary_copy":
            if not real_binary or not Path(real_binary).is_file():
                return {"error": f"real_binary not found: {real_binary}"}
            shutil.copy2(real_binary, binary)
            binary.chmod(0o755)
        else:
            setup = ""
            state_json = '{"operationId":98765,"token":"install-token-98765","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            stdout_tail = "INSTALL_COLLECTOR_STDOUT_OK\n"
            stderr_tail = "INSTALL_COLLECTOR_STDERR_OK\n"
            sleep_time = 0

            if case_name == "duplicate_fields":
                state_json = '{"operationId":987650,"operationId":98765,"token":"install-token-98765","state":"Exited","exitStatus":1,"exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "duplicate_token_identical":
                state_json = '{"operationId":98765,"token":"install-token-98765","token":"install-token-98765","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "wrong_id":
                state_json = '{"operationId":987650,"token":"install-token-98765","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "wrong_token":
                state_json = '{"operationId":98765,"token":"wrong-token","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "wrong_state":
                state_json = '{"operationId":98765,"token":"install-token-98765","state":"Running","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "wrong_exit_status":
                state_json = '{"operationId":98765,"token":"install-token-98765","state":"Exited","exitStatus":1,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "wrong_type_id":
                state_json = '{"operationId":"98765","token":"install-token-98765","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "malformed_json":
                state_json = 'not-json "operationId": 98765'
            elif case_name == "missing_stdout_marker":
                stdout_tail = "WRONG_MARKER\n"
            elif case_name == "missing_stderr_marker":
                stderr_tail = "WRONG_MARKER\n"
            elif case_name == "oversized_state":
                state_json = '{"operationId":98765,"token":"' + ('A' * 5000) + '","state":"Exited","exitStatus":0,"collectorPid":1234,"stdoutTruncated":false,"stderrTruncated":false,"stdoutBytes":30,"stderrBytes":30,"bufferPeak":30,"filePeak":30}'
            elif case_name == "oversized_tail":
                stdout_tail = "INSTALL_COLLECTOR_STDOUT_OK\n" + ("A" * 70000)
            elif case_name == "pre_handshake_stall":
                sleep_time = 30
            elif case_name == "post_handshake_stall":
                sleep_time = 30
            elif case_name == "stalled_with_descendant":
                setup = f"""child = os.fork()
if child == 0:
    time.sleep(30)
    os._exit(0)
Path({str(descendant_pid_file)!r}).write_text(str(child))
"""
                sleep_time = 30
            elif case_name == "descendant_inherits_fd":
                setup = f"""child = os.fork()
if child == 0:
    time.sleep(30)
    os._exit(0)
Path({str(descendant_pid_file)!r}).write_text(str(child))
os._exit(42)
"""
            elif case_name == "crash_before_handshake":
                setup = "os._exit(77)\n"

            code_body = f"""#!/usr/bin/env python3
import os, sys, time
from pathlib import Path

d = Path(sys.argv[sys.argv.index('--op-dir') + 1])
fd = int(sys.argv[sys.argv.index('--handshake-fd') + 1])

{setup}
"""
            if case_name == "pre_handshake_stall":
                code_body += "time.sleep(30)\n"

            if case_name != "crash_before_handshake" and case_name != "descendant_inherits_fd":
                code_body += "os.write(fd, b'R')\n"
                if sleep_time > 0:
                    code_body += f"time.sleep({sleep_time})\n"
                else:
                    code_body += f"""(d / 'state.json').write_text({state_json!r})
(d / 'stdout.tail').write_text({stdout_tail!r})
(d / 'stderr.tail').write_text({stderr_tail!r})
"""
            binary.write_text(code_body)
            binary.chmod(0o755)

        script = root / "gate.sh"
        script.write_text("set -euo pipefail\n" + block)
        script.chmod(0o755)

        env = os.environ.copy()
        env["test_root"] = str(root)
        env["MODERNIME_PREFIX"] = str(prefix)

        if case_name in ("stalled_clock_not_advancing", "wall_clock_frozen"):
            bins = root / "mock-bin"
            bins.mkdir(parents=True, exist_ok=True)
            mock_date = bins / "date"
            mock_date.write_text('#!/bin/sh\nprintf "1000\\n"\n')
            mock_date.chmod(0o755)
            env["PATH"] = f"{bins}:{env.get('PATH', '/usr/bin:/bin')}"
            # Also make binary stall post-handshake
            binary.write_text("""#!/usr/bin/env python3
import os, sys, time
fd = int(sys.argv[sys.argv.index('--handshake-fd') + 1])
os.write(fd, b'R')
time.sleep(30)
""")
            binary.chmod(0o755)

        start_mono = time.monotonic()
        with open(root / "stdout", "w") as so, open(root / "stderr", "w") as se:
            proc = subprocess.Popen(
                ["bash", str(script)],
                env=env,
                stdout=so,
                stderr=se,
                start_new_session=True
            )
            try:
                exit_code = proc.wait(timeout=7.0)
                timed_out = False
            except subprocess.TimeoutExpired:
                exit_code = None
                timed_out = True
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()

        elapsed_ms = round((time.monotonic() - start_mono) * 1000)
        descendant_alive = False
        if descendant_pid_file.exists():
            try:
                child_pid = int(descendant_pid_file.read_text().strip())
                stat_file = Path(f"/proc/{child_pid}/stat")
                if stat_file.exists():
                    state_char = stat_file.read_text().split(") ")[1][0]
                    descendant_alive = state_char not in ("Z", "X")
            except Exception:
                pass

        return {
            "case": case_name,
            "exit": exit_code,
            "timed_out": timed_out,
            "elapsed_ms": elapsed_ms,
            "descendant_alive": descendant_alive,
            "stderr": (root / "stderr").read_text(encoding="utf-8", errors="replace")
        }

def main():
    block = extract_gate_block()
    real_binary = REPO_ROOT / "build" / "architecture-debug" / "tools" / "settings" / "modernime-settings"

    cases = [
        "real_binary_copy",
        "duplicate_fields",
        "duplicate_token_identical",
        "wrong_id",
        "wrong_token",
        "wrong_state",
        "wrong_exit_status",
        "wrong_type_id",
        "malformed_json",
        "missing_stdout_marker",
        "missing_stderr_marker",
        "oversized_state",
        "oversized_tail",
        "pre_handshake_stall",
        "post_handshake_stall",
        "stalled_with_descendant",
        "descendant_inherits_fd",
        "stalled_clock_not_advancing",
        "crash_before_handshake",
    ]

    print(f"Running install gate test matrix ({len(cases)} cases)...")
    results = {}
    any_failed = False

    for c in cases:
        t0 = time.monotonic()
        res = run_gate_test(c, block, real_binary)
        dur = round(time.monotonic() - t0, 2)
        results[c] = res

        # Assertion rules
        if c == "real_binary_copy":
            ok = (res["exit"] == 0 and not res["timed_out"])
        elif "stalled" in c or "stall" in c:
            ok = (res["exit"] != 0 and not res["timed_out"] and not res["descendant_alive"] and 4500 <= res["elapsed_ms"] <= 6500)
        else:
            ok = (res["exit"] != 0 and not res["timed_out"] and not res["descendant_alive"] and res["elapsed_ms"] < 2500)

        status_str = "PASS" if ok else "FAIL"
        if not ok:
            any_failed = True
        print(f"  [{status_str}] {c:30s} -> exit={res['exit']} elapsed={res['elapsed_ms']}ms descendant_alive={res['descendant_alive']}")

    out_dir = REPO_ROOT / "build" / "t13-fifth-remediation-evidence"
    if out_dir.is_dir():
        out_file = out_dir / "install-gate-results.json"
        out_file.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    return 0 if not any_failed else 1

if __name__ == "__main__":
    sys.exit(main())
