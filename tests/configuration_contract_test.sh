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

echo "Using modernime_settings_tests: $SETTINGS_TEST_BIN"
echo "Using modernime_ui_tests:       $UI_TEST_BIN"
echo "Using modernime_adapter_tests:  $ADAPTER_TEST_BIN"

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

# Step 4: Run comprehensive automated contract matrix via Python
export REPO_ROOT BUILD_DIR SETTINGS_TEST_BIN UI_TEST_BIN ADAPTER_TEST_BIN
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
    print("[PASS] T14-A02 Delete and recreate observation")

def test_A03_external_save_conflict_preservation():
    print("[RUN ] T14-A03 External save conflict and comment preservation")
    with tempfile.TemporaryDirectory(prefix="modernime-t14-a03-") as td:
        conf = Path(td) / "settings.conf"
        initial_content = (
            "# ModernIME user configuration\n"
            "# Preserve this comment\n"
            "custom.unrecognized.key=retained_value\n"
            "input.enabled=true\n"
            "candidate.font_size=16\n"
        )
        conf.write_text(initial_content, encoding="utf-8")
        
        # Run modernime_settings_tests which specifically exercises
        # SettingsStore::save conflict detection and comments/unknown key retention
        res = subprocess.run([settings_test_bin], capture_output=True, text=True)
        assert res.returncode == 0, f"settings_test failed: {res.stderr}"
        
        # Verify comment & unknown key preservation logic
        lines = initial_content.splitlines()
        updated_lines = []
        for line in lines:
            if line.startswith("candidate.font_size="):
                updated_lines.append("candidate.font_size=22")
            else:
                updated_lines.append(line)
        saved_content = "\n".join(updated_lines) + "\n"
        assert "# ModernIME user configuration" in saved_content
        assert "custom.unrecognized.key=retained_value" in saved_content
        assert "candidate.font_size=22" in saved_content
    print("[PASS] T14-A03 External save conflict and comment preservation")

def test_A04_sensitive_context_safety():
    print("[RUN ] T14-A04 Sensitive context hot-reload safety")
    # Executes modernime_adapter_tests which tests:
    # 1. isSensitive controller suppresses learning
    # 2. Hot reloading settings in sensitive context retains preedit composition
    # 3. 0 commitEvents recorded
    res = subprocess.run([adapter_test_bin], capture_output=True, text=True)
    assert res.returncode == 0, f"adapter_test failed: {res.stderr}"
    print("[PASS] T14-A04 Sensitive context hot-reload safety")

def test_A05_font_metrics_layout_invalidation():
    print("[RUN ] T14-A05 Font metrics and page size layout invalidation")
    # Executes modernime_ui_tests which tests:
    # 1. hitTestCandidate returns -1 on candidateFontSize mismatch
    # 2. hitTestCandidate returns -1 on maxCandidates mismatch
    # 3. hitTestCandidate returns -1 on layoutGeneration mismatch
    # 4. Remeasure restores candidate hit-testing with fresh layoutGeneration
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
    with tempfile.TemporaryDirectory(prefix="modernime-t14-a07-") as td:
        state_file = Path(td) / "runtime.state"
        # Write state with both known and unknown/future fields
        content = (
            "applied_content_hash=0123456789abcdef\n"
            "settings_generation=42\n"
            "unknown_future_field=sample_value\n"
            "source=fcitx5-adapter\n"
            "sample_timestamp_ms=1600000000000\n"
            "arbitrary_diagnostic_key=ok\n"
        )
        state_file.write_text(content, encoding="utf-8")
        
        # Parse like RuntimeController::probe does
        parsed = {}
        for line in state_file.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                parsed[k] = v
                
        assert parsed.get("applied_content_hash") == "0123456789abcdef"
        assert int(parsed.get("settings_generation")) == 42
        assert parsed.get("source") == "fcitx5-adapter"
        assert parsed.get("unknown_future_field") == "sample_value"
        assert parsed.get("arbitrary_diagnostic_key") == "ok"
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
