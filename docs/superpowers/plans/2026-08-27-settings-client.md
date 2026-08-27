# ModernIME Settings Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and install a GTK3 desktop application that configures ModernIME behavior, manages its local dictionary and learning data, and reports or reloads the running Fcitx5 state.

**Architecture:** Add a small, dependency-light settings/data layer under `core/`, make the Fcitx5 adapter and pinyin provider consume the same validated settings, and keep the GTK3 application in `tools/settings/`. The client edits only ModernIME-owned user files, uses the existing `UserDictionary` and `LearningStore` APIs, and communicates with Fcitx5 through an isolated runtime controller.

**Tech Stack:** C++20, CMake, GTK3/GLib, existing Fcitx5 and LibIME integrations, SQLite3, POSIX filesystem operations.

**Spec:** `docs/superpowers/specs/2026-08-27-settings-client-design.md`

## Global Constraints

- Preserve the existing candidate-bar dimensions, position, fonts, corner radii, and spacing exactly.
- Store configuration under `${XDG_CONFIG_HOME:-$HOME/.config}/modernime/settings.conf`.
- Store ModernIME data under `${XDG_DATA_HOME:-$HOME/.local/share}/modernime/`.
- Never require `sudo`, modify `/usr`, modify another input method, or execute user input through a shell.
- Treat missing files as empty/default state and convert malformed values into per-key defaults plus diagnostics.
- Use atomic configuration and dictionary writes; never destroy the original before the replacement is ready.
- Perform destructive learning reset only after confirmation and after a successful same-directory backup.
- Use GTK main-thread-safe callbacks; no unbounded file or subprocess operation may block the UI thread.
- Every behavior change follows TDD: write one failing test, run it and observe the expected failure, implement minimally, rerun the focused test and the full suite.
- Every independently useful feature ends with its own Git commit. Do not stage the pre-existing untracked `include/` or `src/` directories.

---

### Task 1: Add validated shared settings and user-path resolution

**Files:**
- Create: `core/include/modernime/core/settings.h`
- Create: `core/src/settings.cpp`
- Create: `tests/core/settings_test.cpp`
- Modify: `core/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces `modernime::core::ModernIMESettings` with these fields: `inputEnabled`, `defaultMode` (`Chinese` or `English`), `toggleKey`, `numberSelection`, `arrowNavigation`, `pageNavigation`, `learningEnabled`, and `contextLearningEnabled`.
- Produces `modernime::core::SettingsPaths::fromEnvironment(std::string_view xdgConfigHome, std::string_view xdgDataHome, std::string_view home)`, returning `settingsFile`, `userDictionary`, and `learningStore` paths.
- Produces `modernime::core::SettingsLoadResult { ModernIMESettings settings; std::vector<std::string> diagnostics; }`.
- Produces `modernime::core::SettingsStore::load(const std::filesystem::path &)`, `save(const std::filesystem::path &, const ModernIMESettings &, std::string *error)`, and `reset(const std::filesystem::path &, std::string *error)`.

- [ ] **Step 1: Write the failing settings tests.**

Add tests that assert:

```cpp
const auto paths = SettingsPaths::fromEnvironment("/tmp/cfg", "/tmp/data", "/tmp/home");
assert(paths.settingsFile == "/tmp/cfg/modernime/settings.conf");
assert(paths.userDictionary == "/tmp/data/modernime/user-dictionary.txt");
assert(paths.learningStore == "/tmp/data/modernime/learning.sqlite3");
```

Also test that a missing file returns the documented defaults, a saved file round-trips every field, an unknown key is ignored with a diagnostic, an invalid boolean/mode/key falls back only that field, and a failed save leaves the old file unchanged.

- [ ] **Step 2: Run the focused test and verify the correct failure.**

Run:

```bash
cmake --build build/fcitx5-debug --target modernime_core_tests modernime_settings_tests -j2
```

Expected: configuration fails because the settings interfaces and test target do not exist yet. If the existing build directory has not been regenerated, configure it after adding only the test target and confirm the test fails for the missing implementation rather than for a build typo.

- [ ] **Step 3: Implement the settings model and parser.**

Use a line-oriented `key=value` format with comments beginning with `#`. Use the exact keys from the spec. Define defaults in one function, parse booleans as only `true`/`false`, parse `default_mode` as only `chinese`/`english`, and accept `toggle_key` only when it is a non-empty bounded ASCII key description. Return diagnostics for malformed or unknown lines without throwing.

Resolve empty XDG values by falling back to `home`; reject an empty effective home instead of inventing a relative path. Write to `<settings>.tmp`, close and verify the stream, then rename it over the destination. Create only the parent directory owned by ModernIME.

- [ ] **Step 4: Run the focused tests and the existing core tests.**

Run:

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_settings|modernime_core_foundation' --output-on-failure
```

Expected: all selected tests pass and malformed input never aborts the process.

- [ ] **Step 5: Commit the shared settings feature.**

```bash
git add core/include/modernime/core/settings.h core/src/settings.cpp tests/core/settings_test.cpp core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add validated ModernIME settings"
```

---

### Task 2: Make the engine and pinyin provider consume settings

**Files:**
- Modify: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Modify: `adapter/fcitx5/src/inputmethod.cpp`
- Modify: `algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `tests/adapter/engine_state_test.cpp`
- Modify: `tests/adapter/key_translation_test.cpp`
- Modify: `tests/adapter/pinyin_engine_integration_test.cpp`
- Modify: `tests/core/pinyin_provider_test.cpp`

**Interfaces:**
- `ModernIMEController` consumes a `ControllerOptions` containing `inputEnabled`, `numberSelection`, `arrowNavigation`, and `pageNavigation`.
- `translateKey(const fcitx::Key &, const KeyBindings &)` honors the configured toggle key and returns no navigation event when that operation is disabled.
- `PinyinDataPaths` gains `settingsFile`, `learningEnabled`, and `contextLearningEnabled` behavior through an explicit provider options object; it does not read settings from global mutable state.
- `PinyinCandidateProvider` skips learning reads/writes when learning is disabled and omits surrounding context when contextual learning is disabled.

- [ ] **Step 1: Write failing behavior tests.**

Add tests proving a disabled input controller does not consume alphabetic keys, disabled number selection leaves digit events unhandled, disabled arrow/page navigation leaves those keys unhandled, and the provider does not create or update a learning database when learning is disabled. Add a context test proving context is not passed into ranking when contextual learning is disabled.

- [ ] **Step 2: Run the focused tests and observe failure.**

Run:

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_engine_state|modernime_key_translation|modernime_pinyin_provider|modernime_pinyin_engine' --output-on-failure
```

Expected: the new assertions fail because the engine currently hard-codes the key behavior and the provider always opens the learning writer.

- [ ] **Step 3: Implement explicit options at the adapter/provider boundaries.**

Load `ModernIMESettings` once when `ModernIMEInputMethod` is constructed, pass immutable options into each input-context state, and use the configured values in `ModernIMEController` and `translateKey`. Keep the existing Ctrl+Space default. Do not change candidate-page size or any renderer metrics.

Pass learning flags into `PinyinCandidateProvider` and guard both snapshot reads and selection/suppression writes. When contextual learning is disabled, pass empty before/after strings while retaining ordinary phrase frequency learning if that remains enabled.

- [ ] **Step 4: Run focused and full tests.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_engine_state|modernime_key_translation|modernime_pinyin_provider|modernime_pinyin_engine' --output-on-failure
ctest --test-dir build/fcitx5-debug --output-on-failure
```

Expected: new option tests and all existing tests pass.

- [ ] **Step 5: Commit the runtime settings feature.**

```bash
git add adapter/fcitx5 algorithm/pinyin tests/adapter tests/core
git commit -m "feat: apply settings to ModernIME runtime"
```

---

### Task 3: Build the GTK3 settings application shell and basic page

**Files:**
- Create: `tools/settings/settings_app.cpp`
- Create: `tools/settings/settings_window.h`
- Create: `tools/settings/settings_window.cpp`
- Create: `tests/settings/settings_window_model_test.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `SettingsWindowModel` owns a loaded `ModernIMESettings`, exposes `dirty()`, `setSettings()`, `save()`, `resetEdits()`, and `validationError()`.
- `SettingsWindow` builds a GTK3 window with a left category list and right content stack, and exposes `showBasicPage()`, `showCandidatePage()`, `showLearningPage()`, `showDictionaryPage()`, `showStatusPage()`, and `presentError(std::string_view)`.
- Executable target name is `modernime-settings`; it reads `SettingsPaths::fromEnvironment`, never runs as an Fcitx5 addon, and supports `--version` by printing the project version and exiting successfully.

- [ ] **Step 1: Write the failing model/UI contract tests.**

Test the model starts with defaults, becomes dirty after changing the basic mode, returns clean after a successful save, preserves edits after a save error, and resets edits to the last loaded values. Keep these tests free of a live display server by testing the model separately from GTK widget construction.

- [ ] **Step 2: Run the focused test and observe failure.**

```bash
cmake --build build/fcitx5-debug --target modernime_settings_model_tests -j2
```

Expected: the model target or required interfaces are absent and the test fails before any GUI code is written.

- [ ] **Step 3: Implement the GTK3 shell.**

Create a single top-level window with a `GtkStackSidebar`/`GtkStack` pair, a bottom action row containing `保存`, `应用`, and `恢复修改`, and a status label for save/reload messages. The basic page contains controls for input enabled, default Chinese/English mode, and toggle key. Bind controls to `SettingsWindowModel`; do not add candidate geometry controls.

Use `gtk_application_new` and a single-instance application ID such as `com.modernime.Settings`. On activation, present the existing window instead of creating a second one. Use UTF-8-safe GTK labels and keep all file work outside the widget callbacks.

- [ ] **Step 4: Run the model tests, headless application smoke test, and full suite.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_settings_model' --output-on-failure
GDK_BACKEND=x11 xvfb-run -a ./build/fcitx5-debug/tools/modernime-settings --version
ctest --test-dir build/fcitx5-debug --output-on-failure
```

Expected: the application starts and exits cleanly for `--version`, model tests pass, and no existing test regresses.

- [ ] **Step 5: Commit the settings shell.**

```bash
git add CMakeLists.txt tools tests/settings tests/CMakeLists.txt
git commit -m "feat: add ModernIME settings application"
```

---

### Task 4: Add candidate controls and Fcitx5 runtime status/reload

**Files:**
- Create: `tools/settings/runtime_controller.h`
- Create: `tools/settings/runtime_controller.cpp`
- Create: `tests/settings/fake_fcitx5_remote.sh`
- Modify: `tools/settings/settings_window.h`
- Modify: `tools/settings/settings_window.cpp`
- Create: `tests/settings/runtime_controller_test.cpp`
- Modify: `tests/settings/settings_window_model_test.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- `RuntimeStatus { bool available; bool running; std::string currentInputMethod; bool modernimeActive; std::string message; }`.
- `RuntimeController::probe(const std::filesystem::path &executable, const Environment &environment) -> RuntimeStatus`.
- `RuntimeController::reload(const std::filesystem::path &executable, const Environment &environment) -> RuntimeResult`.
- `Environment` is an explicit vector of name/value pairs containing inherited `DISPLAY`, `DBUS_SESSION_BUS_ADDRESS`, and `XDG_RUNTIME_DIR`; no shell string is accepted.
- The fake test executable accepts `-n`, status, and reload arguments and returns controlled stdout/exit-code cases; production code never invokes it or any command through a shell.

- [ ] **Step 1: Write failing runtime and candidate-page tests.**

Use a test-only executable path to assert that missing `fcitx5-remote`, non-zero exit status, and valid `-n`/status output become distinct user-readable states. Add model assertions that candidate number selection, arrow navigation, and page navigation can be changed and persisted without changing any UI geometry value.

- [ ] **Step 2: Run focused tests and observe failure.**

```bash
cmake --build build/fcitx5-debug --target modernime_runtime_controller_tests modernime_settings_model_tests -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_runtime_controller|modernime_settings_model' --output-on-failure
```

Expected: the controller interfaces and candidate settings controls do not exist yet.

- [ ] **Step 3: Implement argv-based runtime communication.**

Use GLib `GSubprocess` with an argument vector for `fcitx5-remote -n`, status, and reload. Capture stdout/stderr, impose a bounded timeout, and map unavailable executable, timeout, non-zero exit, and success to `RuntimeResult`. Run the blocking subprocess work on a worker and marshal the result back to the GTK main context.

Add the candidate page with only behavioral toggles. The status page displays Fcitx5 state, active input method, ModernIME state, and a `重新加载 ModernIME` action. A reload action must refresh the status and show an error without discarding unsaved local edits.

- [ ] **Step 4: Run focused, full, and headless GUI tests.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_runtime_controller|modernime_settings_model' --output-on-failure
GDK_BACKEND=x11 xvfb-run -a ./build/fcitx5-debug/tools/modernime-settings --version
ctest --test-dir build/fcitx5-debug --output-on-failure
```

- [ ] **Step 5: Commit candidate/runtime settings.**

```bash
git add tools tests/settings
git commit -m "feat: add candidate and runtime settings"
```

---

### Task 5: Add safe learning controls and user dictionary management

**Files:**
- Create: `tools/settings/data_controller.h`
- Create: `tools/settings/data_controller.cpp`
- Modify: `core/include/modernime/core/learning_store.h`
- Modify: `core/src/learning_store.cpp`
- Modify: `algorithm/pinyin/include/modernime/pinyin/user_dictionary.h`
- Modify: `algorithm/pinyin/src/user_dictionary.cpp`
- Modify: `tools/settings/settings_window.h`
- Modify: `tools/settings/settings_window.cpp`
- Create: `tests/settings/data_controller_test.cpp`
- Modify: `tests/core/learning_store_test.cpp`
- Modify: `tests/core/user_dictionary_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- `DataController::loadDictionary(path) -> std::vector<UserDictionaryEntry>`.
- `DataController::saveDictionary(path, entries, error) -> bool` using `UserDictionary` validation and atomic save.
- `UserDictionary::upsert(std::string_view pinyin, std::string_view phrase, float weight) -> bool` validates and replaces by normalized pinyin plus phrase.
- `LearningStore::backupTo(const std::filesystem::path &path) -> bool` uses SQLite's online backup API so WAL contents are included.
- `DataController::backupAndClearLearning(path, backupPath, error) -> bool` calls `LearningStore::backupTo` and then `LearningStore::clear`, verifying the backup before the clear.
- `LearningStore::clear()` clears only ModernIME learning rows while preserving the schema and returning false on a failed transaction.

- [ ] **Step 1: Write failing data-management tests.**

Test adding/editing/removing a valid user dictionary entry, rejection of invalid pinyin/UTF-8, import of a valid file into a temporary target, export round-trip, and duplicate replacement by normalized pinyin plus phrase. Test learning reset creates a backup before clearing, leaves the backup readable, produces an empty snapshot, and leaves the original untouched when backup creation fails.

- [ ] **Step 2: Run focused tests and observe failure.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_learning_store|modernime_user_dictionary|modernime_data_controller' --output-on-failure
```

Expected: the clear/backup and editor operations are absent, so the new assertions fail before implementation.

- [ ] **Step 3: Implement transactional learning reset and dictionary editing.**

Add `LearningStore::backupTo()` using `sqlite3_backup`, then add `LearningStore::clear()` with `BEGIN IMMEDIATE`, `DELETE FROM learning_entries`, `COMMIT`, and rollback on every failure. In `DataController`, create a timestamped backup in the learning database directory, verify it can be opened and snapshotted, and only then call `clear`. Reuse `UserDictionary::loadText`, `remove`, and `saveText`; implement `UserDictionary::upsert` so GTK code does not duplicate parser or normalization logic.

- [ ] **Step 4: Implement the learning and dictionary pages.**

The learning page contains the two learning toggles, database path display, and a `清空学习记录` button. The dictionary page contains a list with pinyin/phrase/weight columns and add/edit/delete/import/export actions. Require confirmation for delete and learning reset. Import into a temporary parsed vector and replace the live file only after all rows pass validation.

- [ ] **Step 5: Run focused, full, and headless tests.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_learning_store|modernime_user_dictionary|modernime_data_controller' --output-on-failure
GDK_BACKEND=x11 xvfb-run -a ./build/fcitx5-debug/tools/modernime-settings --version
ctest --test-dir build/fcitx5-debug --output-on-failure
```

- [ ] **Step 6: Commit learning and dictionary management.**

```bash
git add core algorithm/pinyin tools tests
git commit -m "feat: manage ModernIME learning and dictionary data"
```

---

### Task 6: Add desktop installation, reset-default behavior, and end-to-end verification

**Files:**
- Create: `config/desktop/modernime-settings.desktop`
- Modify: `install.sh`
- Modify: `uninstall.sh`
- Modify: `tools/settings/settings_window.cpp`
- Create: `tests/settings/settings_install_test.sh`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/install_environment_test.sh`

**Interfaces:**
- `install.sh` installs `modernime-settings` to `$prefix/bin`, installs the desktop file under `$prefix/share/applications`, and records both in the existing manifest.
- `uninstall.sh` removes only manifest entries and leaves unrelated files untouched.
- Reset defaults writes `SettingsStore::defaults()` through the same atomic save path and optionally clears only ModernIME learning data after confirmation.

- [ ] **Step 1: Write failing installation/reset tests.**

Extend the fake-command installation test to assert the application binary and desktop entry are included in the manifest and that no `sudo` command is emitted. Add a reset test proving only ModernIME settings are replaced and unrelated files remain byte-for-byte unchanged.

- [ ] **Step 2: Run focused tests and observe failure.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug -R 'modernime_install_environment|modernime_settings_install' --output-on-failure
```

Expected: the installer does not yet know about the settings executable or desktop entry.

- [ ] **Step 3: Implement install/uninstall and reset defaults.**

Add the executable and desktop file to CMake installation. Update `install.sh` manifest generation and the autostart-independent settings application installation path. Update `uninstall.sh` only through the recorded manifest. Add a reset-default confirmation flow that saves defaults without touching candidate-bar geometry or other input methods.

- [ ] **Step 4: Run all verification layers.**

```bash
cmake --build build/fcitx5-debug -j2
ctest --test-dir build/fcitx5-debug --output-on-failure
cmake --build /tmp/modernime-werror -j2
ctest --test-dir /tmp/modernime-werror --output-on-failure
cmake --build /tmp/modernime-asan -j2
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir /tmp/modernime-asan --output-on-failure
git diff --check
```

Then run the user-scoped installer with the current desktop environment, launch `modernime-settings`, verify all five pages, save a setting, reload Fcitx5, and confirm ModernIME still accepts input and displays the unchanged candidate bar. Record any environment-specific limitation instead of weakening tests.

- [ ] **Step 5: Commit installation and release integration.**

```bash
git add config/desktop install.sh uninstall.sh tools tests
git commit -m "feat: install ModernIME settings client"
```

## Final Review Checklist

- [ ] `git status --short` shows only the pre-existing untracked `include/` and `src/` directories.
- [ ] `ctest --test-dir build/fcitx5-debug --output-on-failure` passes.
- [ ] Strict-warning and ASan/UBSan suites pass with the documented leak-check limitation.
- [ ] Settings, dictionary, learning, runtime, and install tests cover failure paths as well as success paths.
- [ ] Candidate-bar dimensions and visual constants are unchanged.
- [ ] No installer or client operation requires `sudo` or touches unrelated Fcitx5 data.
