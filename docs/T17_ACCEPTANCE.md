# T17 Acceptance Report: Explicit Install Lifecycle Phase (P2-04)

## Overview & Scope
- **Task**: ModernIME T17「显式安装阶段」(P2-04 Explicit Install Lifecycle Phase).
- **Worktree**: `/home/wsl/ModernIME/.worktrees/parallel-t17-20261017`
- **Branch**: `codex/architecture-t17-20261017`
- **Base Integration**: Synced cleanly with T14 path remediation (`a5dbf742`).
- **Core Invariant**: `RollbackPreservesConcurrentUserWrites` strictly upheld across upgrade failures and rollback.

---

## Architecture & Implementation Highlights

### 1. Explicit Lifecycle Library (`scripts/install_lifecycle.sh`)
- Implemented `modernime_deploy_payload PAYLOAD PREFIX TRANSACTION_DIR`:
  - **Phase 1: Pre-flight Verification & Backup**: Inspects payload collision risks (e.g. payload destination colliding with directory) and backs up pre-existing files into `TRANSACTION_DIR/backup`.
  - **Phase 2: Atomic Deployment with Inode Protection**: Deploys payload items individually using `cp -a --remove-destination` (unlink before copy) so active running processes (e.g. Fcitx5 loading `modernime_fcitx5.so`) retain their mapped inodes and do not crash.
  - **Phase 3: Safe Rollback**: `modernime_rollback_payload PREFIX TRANSACTION_DIR` removes newly deployed files and restores backups. Crucially, rollback touches **only** payload files and never touches user state or live writes (`RollbackPreservesConcurrentUserWrites`).
- Implemented `modernime_integrate PREFIX CONFIG_HOME DESKTOP_DIR MANAGED_STATE_DIR [DATA_HOME]`:
  - Centralizes desktop shortcut generation, `application/modernime-settings.desktop` generation, `environment.d/90-modernime.conf`, autostart desktop entry, `~/.xprofile` environment block, `install-manifest.txt`, and `install-channel.txt`.
  - Seamlessly integrates T14's `XDG_DATA_HOME` forwarding rules for custom paths.
  - Passes explicit `MANAGED_STATE_DIR` throughout all managed file helpers to avoid global variable leakage.

### 2. Symmetrical Uninstallation (`uninstall.sh`) & State Management (`scripts/install_state.sh`)
- Updated `scripts/install_state.sh` functions (`modernime_write_managed_file`, `modernime_remove_managed_file`, `modernime_write_profile_block`, `modernime_remove_profile_block`) to accept explicit `managed_state_dir` parameter with backwards-compatible fallback.
- Updated `uninstall.sh` to track `install_lifecycle.sh` in the manifest and pass explicit `"$managed_state_dir"`.

### 3. Binary Installer Generator (`scripts/generate_binary_installer.sh`)
- Eliminated legacy brittle `awk` line-text slicing of `install.sh`.
- Emits a clean, explicit thin wrapper installer that sources `scripts/install_state.sh` and `scripts/install_lifecycle.sh`, embeds the verified bundled version, and invokes `modernime_deploy_payload` and `modernime_integrate`.
- Packages all runtime scripts (`install_lifecycle.sh`, `install_state.sh`, `modernime-data-guard`, `modernime-migrate-channel`).

### 4. CMake Staging Installation
- Updated `tools/settings/CMakeLists.txt` to install `scripts/install_lifecycle.sh` into `${CMAKE_INSTALL_DATADIR}/modernime`.

---

## Verification & Test Results

All tests executed with mutual exclusion under `flock -x /home/wsl/ModernIME/build/parallel-control-20261017/machine.lock`:

1. `tests/install_lifecycle_prep_test.sh`: **PASS** (4/4 test cases)
   - Test 1: `RollbackPreservesConcurrentUserWrites` with simulated failure, SQLite WAL, configs, and custom dict.
   - Test 2: Pre-flight Destination Collision Protection.
   - Test 3: Clean Deployment Success & destination unlinking verification.
   - Test 4: `modernime_integrate` & user edit protection.
2. `tests/install_rollback_concurrent_writes_test.sh`: **PASS** (3/3 test cases)
   - Baseline Anti-Example data loss detection.
   - Space prefix and active WAL preservation under simulated failure.
   - `modernime_deploy_payload` lifecycle rollback under concurrent writes.
3. `tests/install_transaction_test.sh`: **PASS**
   - Verified rollback, fresh, backup, deploy, stage, success, binary-success, learning, binary-learning, link-preexisting, link-created, and link-modified scenarios.
4. `tests/install_runtime_test.sh`: **PASS**
   - Source & binary install runtime verification and temporary-directory safety guards.
5. `tests/install_environment_test.sh`: **PASS**
   - Environment and desktop session configurations.
6. `tests/upgrade_data_protection_test.sh`: **PASS**
   - Online backup snapshot writer and SQLite/LMDB live database preservation.
7. `tests/install_channel_collision_test.sh`: **PASS**
   - Package manager collision detection (`dpkg`/`rpm`) and legacy channel migration.
