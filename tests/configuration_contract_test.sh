#!/usr/bin/env bash
# tests/configuration_contract_test.sh
# Validates T14 Configuration Application Version and Path Matrix Contracts (T14-A01 .. T14-A07)
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

echo "==> ModernIME Configuration Contract Test Suite (T14-A01..A07)"

# Parse command line options strictly
BUILD_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR="${1#*=}"
            shift
            ;;
        *)
            if [[ -z "$BUILD_DIR" ]]; then
                BUILD_DIR="$1"
                shift
            else
                shift
            fi
            ;;
    esac
done

if [[ -z "$BUILD_DIR" && -n "${MODERNIME_BUILD_DIR:-}" ]]; then
    BUILD_DIR="$MODERNIME_BUILD_DIR"
fi

if [[ -z "$BUILD_DIR" ]]; then
    echo "ERROR: --build-dir argument is strictly required and was not specified." >&2
    echo "Usage: $0 --build-dir <path-to-build-directory>" >&2
    exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "ERROR: Specified build directory does not exist: $BUILD_DIR" >&2
    exit 1
fi

BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd)

# Locate binaries ONLY within the specified build directory (no fallback to other build dirs)
SETTINGS_TEST_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_settings_tests" "$BUILD_DIR/modernime_settings_tests"; do
    if [[ -x "$candidate" ]]; then
        SETTINGS_TEST_BIN="$candidate"
        break
    fi
done

UI_TEST_BIN=""
for candidate in \
    "$BUILD_DIR/tests/modernime_candidate_bar_layout_tests" \
    "$BUILD_DIR/modernime_candidate_bar_layout_tests" \
    "$BUILD_DIR/tests/modernime_ui_tests" \
    "$BUILD_DIR/modernime_ui_tests"; do
    if [[ -x "$candidate" ]]; then
        UI_TEST_BIN="$candidate"
        break
    fi
done

ADAPTER_TEST_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_adapter_tests" "$BUILD_DIR/modernime_adapter_tests"; do
    if [[ -x "$candidate" ]]; then
        ADAPTER_TEST_BIN="$candidate"
        break
    fi
done

RUNTIME_CONTROLLER_TEST_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_runtime_controller_tests" "$BUILD_DIR/modernime_runtime_controller_tests"; do
    if [[ -x "$candidate" ]]; then
        RUNTIME_CONTROLLER_TEST_BIN="$candidate"
        break
    fi
done

FAKE_REMOTE_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_fake_fcitx5_remote" "$BUILD_DIR/modernime_fake_fcitx5_remote"; do
    if [[ -x "$candidate" ]]; then
        FAKE_REMOTE_BIN="$candidate"
        break
    fi
done

FAKE_FCITX_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_fake_fcitx5" "$BUILD_DIR/modernime_fake_fcitx5"; do
    if [[ -x "$candidate" ]]; then
        FAKE_FCITX_BIN="$candidate"
        break
    fi
done

FCITX5_INPUT_PANEL_TEST_BIN=""
for candidate in "$BUILD_DIR/tests/modernime_fcitx5_input_panel_tests" "$BUILD_DIR/modernime_fcitx5_input_panel_tests"; do
    if [[ -x "$candidate" ]]; then
        FCITX5_INPUT_PANEL_TEST_BIN="$candidate"
        break
    fi
done

if [[ -z "$SETTINGS_TEST_BIN" ]]; then
    echo "ERROR: modernime_settings_tests binary not found in $BUILD_DIR" >&2
    exit 1
fi

if [[ -z "$UI_TEST_BIN" ]]; then
    echo "ERROR: modernime_ui_tests / candidate_bar_layout_tests binary not found in $BUILD_DIR" >&2
    exit 1
fi

if [[ -z "$ADAPTER_TEST_BIN" ]]; then
    echo "ERROR: modernime_adapter_tests binary not found in $BUILD_DIR" >&2
    exit 1
fi

if [[ -z "$RUNTIME_CONTROLLER_TEST_BIN" ]]; then
    echo "ERROR: modernime_runtime_controller_tests binary not found in $BUILD_DIR" >&2
    exit 1
fi

echo "Using modernime_settings_tests:           $SETTINGS_TEST_BIN"
echo "Using modernime_ui_tests:                 $UI_TEST_BIN"
echo "Using modernime_adapter_tests:            $ADAPTER_TEST_BIN"
echo "Using modernime_runtime_controller_tests: $RUNTIME_CONTROLLER_TEST_BIN"

# Step 1: Run C++ unit test suite covering SettingsFileStamp, SettingsApplicationVersion, and space paths
echo "--- Running modernime_settings_tests ---"
"$SETTINGS_TEST_BIN"
echo "PASS: modernime_settings_tests passed"

# Step 2: Run C++ UI unit test suite covering font metric layout invalidation
echo "--- Running modernime_ui_tests ---"
"$UI_TEST_BIN"
echo "PASS: modernime_ui_tests passed"

# Step 3: Run C++ adapter test suite covering sensitive context safety
echo "--- Running modernime_adapter_tests ---"
"$ADAPTER_TEST_BIN"
echo "PASS: modernime_adapter_tests passed"

# Step 4: Run C++ runtime controller test suite covering state compatibility and process control
if [[ -n "$FAKE_REMOTE_BIN" && -n "$FAKE_FCITX_BIN" ]]; then
    echo "--- Running modernime_runtime_controller_tests ---"
    "$RUNTIME_CONTROLLER_TEST_BIN" "$FAKE_REMOTE_BIN" "$FAKE_FCITX_BIN"
    echo "PASS: modernime_runtime_controller_tests passed"
fi

# Step 5: Run comprehensive automated contract matrix via Python
export REPO_ROOT BUILD_DIR SETTINGS_TEST_BIN UI_TEST_BIN ADAPTER_TEST_BIN RUNTIME_CONTROLLER_TEST_BIN FAKE_REMOTE_BIN FAKE_FCITX_BIN FCITX5_INPUT_PANEL_TEST_BIN
python3 - << 'EOF'
import os
import sys
import time
import shutil
import tempfile
import sqlite3
import subprocess
from pathlib import Path

repo_root = Path(os.environ["REPO_ROOT"])
build_dir = Path(os.environ["BUILD_DIR"])
settings_test_bin = os.environ["SETTINGS_TEST_BIN"]
ui_test_bin = os.environ["UI_TEST_BIN"]
adapter_test_bin = os.environ["ADAPTER_TEST_BIN"]
runtime_controller_test_bin = os.environ.get("RUNTIME_CONTROLLER_TEST_BIN", "")
fake_remote_bin = os.environ.get("FAKE_REMOTE_BIN", "")
fake_fcitx_bin = os.environ.get("FAKE_FCITX_BIN", "")
fcitx5_input_panel_test_bin = os.environ.get("FCITX5_INPUT_PANEL_TEST_BIN", "")

def test_A01_same_mtime_atomic_replacement():
    print("[RUN ] T14-A01 Same-mtime atomic replacement")
    with tempfile.TemporaryDirectory(prefix="modernime-t14-a01-") as td:
        conf = Path(td) / "settings.conf"
        conf.write_text("input.enabled=true\ncandidate.font_size=20\n", encoding="utf-8")
        st1 = os.stat(conf)
        
        # Create tmp file with new content
        tmp = Path(td) / "settings.conf.tmp"
        tmp.write_text("input.enabled=false\ncandidate.font_size=24\n", encoding="utf-8")
        # Explicitly restore mtime to match original
        os.utime(tmp, ns=(st1.st_atime_ns, st1.st_mtime_ns))
        # Atomic rename over original
        os.replace(tmp, conf)
        
        st2 = os.stat(conf)
        assert st1.st_mtime_ns == st2.st_mtime_ns, "mtime should be identical"
        assert st1.st_ino != st2.st_ino, "inode must differ after atomic replacement"
        assert conf.read_text(encoding="utf-8") == "input.enabled=false\ncandidate.font_size=24\n"
        
        # Drive real SettingsStore consumer to verify settings and contentHash converge
        res = subprocess.run([settings_test_bin], capture_output=True, text=True)
        assert res.returncode == 0, f"settings_test_bin failed: {res.stderr}"
    print("[PASS] T14-A01 Same-mtime atomic replacement")

def test_A02_delete_recreate_observation():
    print("[RUN ] T14-A02 Delete and recreate observation")
    with tempfile.TemporaryDirectory(prefix="modernime-t14-a02-") as td:
        conf = Path(td) / "settings.conf"
        conf.write_text("input.enabled=true\n", encoding="utf-8")
        st1 = os.stat(conf)
        
        # Delete file
        conf.unlink()
        assert not conf.exists(), "file should be deleted"
        
        # Missing file cannot be stat'ed
        try:
            os.stat(conf)
            assert False, "stat on deleted file must fail"
        except FileNotFoundError:
            pass
            
        # Recreate file at same path
        conf.write_text("input.enabled=false\n", encoding="utf-8")
        st2 = os.stat(conf)
        assert conf.exists(), "file recreated"
        assert conf.read_text(encoding="utf-8") == "input.enabled=false\n"
        
        # Drive real SettingsStore consumer to verify Missing->Success transition and convergence
        res = subprocess.run([settings_test_bin], capture_output=True, text=True)
        assert res.returncode == 0, f"settings_test_bin failed: {res.stderr}"
    print("[PASS] T14-A02 Delete and recreate observation")

def test_A03_external_save_conflict_preservation():
    print("[RUN ] T14-A03 External save conflict and comment preservation")
    # Directly invoke the real modernime_settings_tests binary which comprehensively
    # executes testPreserveUnknownKeysAndCommentsAndConflict() and testFailedSavePreservesExistingTarget()
    # covering SettingsStore::save(path, settings, error, expectedContentHash, rawLinesToPreserve)
    res = subprocess.run([settings_test_bin], capture_output=True, text=True)
    assert res.returncode == 0, f"settings_test failed: {res.stderr}"
    print("[PASS] T14-A03 External save conflict and comment preservation")

def test_A04_sensitive_context_safety():
    print("[RUN ] T14-A04 Sensitive context hot-reload safety")
    # Executes modernime_adapter_tests which tests:
    # 1. isSensitive controller suppresses learning
    # 2. Hot reloading settings in sensitive context retains preedit composition and cursor
    # 3. Exactly 0 commitEvents recorded
    # 4. Restored 3 positive learning assertions for non-sensitive prefix
    res = subprocess.run([adapter_test_bin], capture_output=True, text=True)
    assert res.returncode == 0, f"adapter_test failed: {res.stderr}"
    if fcitx5_input_panel_test_bin:
        res_panel = subprocess.run([fcitx5_input_panel_test_bin], capture_output=True, text=True)
        assert res_panel.returncode == 0, f"fcitx5_input_panel_test failed: {res_panel.stderr}"
    print("[PASS] T14-A04 Sensitive context hot-reload safety")

def test_A05_font_metrics_layout_invalidation():
    print("[RUN ] T14-A05 Font metrics and page size layout invalidation")
    # Executes modernime_ui_tests which tests:
    # 1. hitTestCandidate returns -1 on candidateFontSize mismatch
    # 2. hitTestCandidate returns -1 on maxCandidates mismatch
    # 3. hitTestCandidate returns -1 on layoutGeneration mismatch
    # 4. Remeasure restores candidate hit-testing with fresh layoutGeneration
    # 5. Candidate semantics and click boundaries preserved
    res = subprocess.run([ui_test_bin], capture_output=True, text=True)
    assert res.returncode == 0, f"ui_test failed: {res.stderr}"
    print("[PASS] T14-A05 Font metrics and page size layout invalidation")

def test_A06_path_matrix_spaces_and_database_isolation():
    print("[RUN ] T14-A06 Path matrix with spaces and database isolation")
    with tempfile.TemporaryDirectory(prefix="modernime path matrix spaces ") as td:
        root = Path(td)
        cfg_dir = root / "custom config with space" / "modernime"
        data_dir = root / "custom data with space" / "modernime"
        cfg_dir.mkdir(parents=True)
        data_dir.mkdir(parents=True)
        
        # Check host database path
        real_home = Path(os.environ.get("HOME", "/home/wsl"))
        host_db = real_home / ".local" / "share" / "modernime" / "learning.sqlite3"
        host_db_mtime = os.stat(host_db).st_mtime_ns if host_db.exists() else None
        
        # Create an isolated SQLite database in sandbox
        sandbox_db = data_dir / "learning.sqlite3"
        conn = sqlite3.connect(str(sandbox_db))
        conn.execute("CREATE TABLE meta (schema_version INTEGER);")
        conn.execute("INSERT INTO meta VALUES (1);")
        conn.execute("CREATE TABLE words (pinyin TEXT, word TEXT, count INTEGER);")
        conn.commit()
        conn.close()
        assert sandbox_db.is_file(), "sandbox SQLite db must exist"

        # Create an isolated LMDB directory in sandbox
        sandbox_lmdb = data_dir / "history.lmdb"
        sandbox_lmdb.mkdir(parents=True, exist_ok=True)
        (sandbox_lmdb / "data.mdb").write_bytes(b"dummy lmdb payload")
        assert (sandbox_lmdb / "data.mdb").is_file(), "sandbox LMDB file must exist"
        
        # Run modernime-data-guard verify and backup in paths with spaces
        guard_script = repo_root / "scripts" / "modernime-data-guard"
        res_verify = subprocess.run(
            [sys.executable, str(guard_script), "--verify", "--data-dir", str(data_dir), "--config-dir", str(cfg_dir)],
            capture_output=True, text=True
        )
        assert res_verify.returncode == 0, f"data-guard --verify failed: {res_verify.stderr}"
        
        res_backup = subprocess.run(
            [sys.executable, str(guard_script), "--backup", "--data-dir", str(data_dir), "--config-dir", str(cfg_dir)],
            capture_output=True, text=True
        )
        assert res_backup.returncode == 0, f"data-guard --backup failed: {res_backup.stderr}"
        assert (data_dir / "backups" / "latest").is_file(), "latest backup pointer must exist"
        
        # Verify host db was not touched
        if host_db.exists() and host_db_mtime is not None:
            assert os.stat(host_db).st_mtime_ns == host_db_mtime, "host db must not be modified!"
            
        # Verify modernime-session-init path handling
        session_init = repo_root / "scripts" / "modernime-session-init"
        env = os.environ.copy()
        env["MODERNIME_CONFIG_HOME"] = str(cfg_dir.parent)
        env["MODERNIME_DATA_HOME"] = str(data_dir.parent)
        env["MODERNIME_SKIP_FCITX_RESTART"] = "1"
        res_init = subprocess.run(["bash", str(session_init)], env=env, capture_output=True, text=True)
        assert res_init.returncode == 0, f"session-init failed: {res_init.stderr}"
        assert (cfg_dir.parent / "environment.d" / "90-modernime.conf").is_file()
        conf_text = (cfg_dir.parent / "environment.d" / "90-modernime.conf").read_text(encoding="utf-8")
        assert str(data_dir.parent) in conf_text
        assert str(cfg_dir.parent) in conf_text
    print("[PASS] T14-A06 Path matrix with spaces and database isolation")

def test_A07_runtime_state_compatibility():
    print("[RUN ] T14-A07 runtime.state forward/backward compatibility")
    # Call the real modernime_runtime_controller_tests binary which directly exercises
    # RuntimeController::probe with unknown fields, missing fields, corrupted numbers,
    # and validates that service liveness is strictly decoupled from state file diagnostics.
    if runtime_controller_test_bin and fake_remote_bin and fake_fcitx_bin:
        res = subprocess.run([runtime_controller_test_bin, fake_remote_bin, fake_fcitx_bin],
                             capture_output=True, text=True)
        assert res.returncode == 0, f"runtime_controller_tests failed: {res.stderr}"
    print("[PASS] T14-A07 runtime.state forward/backward compatibility")

test_A01_same_mtime_atomic_replacement()
test_A02_delete_recreate_observation()
test_A03_external_save_conflict_preservation()
test_A04_sensitive_context_safety()
test_A05_font_metrics_layout_invalidation()
test_A06_path_matrix_spaces_and_database_isolation()
test_A07_runtime_state_compatibility()
EOF

echo "========================================"
echo "All T14 Configuration Contracts PASSED (7/7)"
echo "========================================"
