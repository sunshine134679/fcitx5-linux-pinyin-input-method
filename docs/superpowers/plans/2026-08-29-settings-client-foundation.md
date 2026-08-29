# ModernIME Settings Client Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver phase one of the approved settings-client redesign: a responsive GTK3 shell with overview, local settings search, consolidated input controls, preserved data-management pages, precise validation, and reliable save/reload feedback.

**Architecture:** Keep the existing core stores and GTK3 dependency, but reduce `settings_window.cpp` to composition and window-level coordination. Put page metadata/search, edit-session behavior, overview data, shared widgets, and each page in focused units with GTK-independent state helpers where behavior needs direct tests.

**Tech Stack:** C++20, GTK3/GIO/GLib, CMake 3.24+, CTest, SQLite3, existing ModernIME core and pinyin libraries.

**Spec:** `docs/superpowers/specs/2026-08-29-settings-client-redesign-design.md`

## Global Constraints

- Only the independent GTK3 settings client changes; do not modify the Fcitx5 candidate bar appearance or interaction.
- Keep GTK3 and the existing `SettingsStore`, `UserDictionary`, `LearningStore`, persistent clipboard format, and `RuntimeController` behavior.
- The client remains completely offline in phase one; do not add network libraries, requests, timers, or unavailable network pages.
- All configuration and data access stays within paths returned by `core::SettingsPaths` for the current user; never require `sudo`.
- Keep long-running data and runtime probes off the GTK main thread and marshal results back through the GTK main context.
- Preserve atomic file writes and current backup-before-clear behavior.
- Do not display an action unless its complete normal, empty, loading, and failure behavior is implemented.
- Every task follows red-green-refactor and ends with a focused Chinese commit.
- Before every commit, run the task-specific test plus `ctest --preset fcitx5-debug` when the plan says to run the complete suite.

---

## Planned File Structure

### New focused units

- `tools/settings/include/modernime/settings/page_registry.h`: stable page identifiers, navigation metadata, search entries, and local search API.
- `tools/settings/page_registry.cpp`: case-folded local matching and ranked search results.
- `tools/settings/include/modernime/settings/settings_widgets.h`: shared GTK3 page, section, row, status-pill, empty-state, and scroller factories.
- `tools/settings/settings_widgets.cpp`: widget construction, CSS installation, spacing, accessibility labels, and theme-safe style classes.
- `tools/settings/include/modernime/settings/overview_model.h`: GTK-independent overview snapshot and notice types.
- `tools/settings/overview_model.cpp`: collect counts and derive overview notices from existing stores and runtime state.
- `tools/settings/include/modernime/settings/pages/input_page.h` and `tools/settings/pages/input_page.cpp`: merged input, shortcut, punctuation, and candidate behavior controls.
- `tools/settings/include/modernime/settings/pages/overview_page.h` and `tools/settings/pages/overview_page.cpp`: overview cards, loading state, notices, and navigation callbacks.
- `tools/settings/include/modernime/settings/pages/clipboard_page.h` and `tools/settings/pages/clipboard_page.cpp`: existing clipboard behavior extracted without phase-two enhancements.
- `tools/settings/include/modernime/settings/pages/learning_page.h` and `tools/settings/pages/learning_page.cpp`: existing learning controls extracted without phase-two enhancements.
- `tools/settings/include/modernime/settings/pages/dictionary_page.h` and `tools/settings/pages/dictionary_page.cpp`: existing dictionary behavior extracted without phase-two enhancements.
- `tools/settings/include/modernime/settings/pages/diagnostics_page.h` and `tools/settings/pages/diagnostics_page.cpp`: current runtime-status behavior under the approved “系统与诊断” label.
- `tools/settings/include/modernime/settings/settings_shell.h` and `tools/settings/settings_shell.cpp`: navigation, stack, search popover, global feedback, and bottom action bar.

### Existing units changed

- `core/include/modernime/core/settings.h` and `core/src/settings.cpp`: expose field-addressable validation issues while keeping settings-file compatibility.
- `tools/settings/include/modernime/settings/settings_model.h` and `tools/settings/settings_model.cpp`: preserve saved/draft settings, edit defaults without immediate persistence, and track runtime reload state.
- `tools/settings/include/modernime/settings/settings_window.h` and `tools/settings/settings_window.cpp`: retain public navigation methods while delegating page and shell work.
- `tools/settings/include/modernime/settings/settings_ui_contract.h`: remove the obsolete six-page array and retain action/status copy used by focused components.
- `tools/settings/CMakeLists.txt` and `tests/CMakeLists.txt`: compile the focused units and their tests.
- `README.md`: describe the new page structure and phase-one behavior.

---

### Task 1: Page registry and local settings search

**Files:**
- Create: `tools/settings/include/modernime/settings/page_registry.h`
- Create: `tools/settings/page_registry.cpp`
- Create: `tests/settings/page_registry_test.cpp`
- Modify: `tools/settings/include/modernime/settings/settings_ui_contract.h`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/settings/settings_ui_contract_test.cpp`

**Interfaces:**
- Consumes: no earlier task interfaces.
- Produces: `SettingsPageId`, `SettingsPageDefinition`, `SettingsSearchEntry`, `settingsPageDefinitions()`, `settingsSearchEntries()`, and `searchSettings(std::string_view)`.

- [ ] **Step 1: Write the failing registry/search test**

```cpp
#include "modernime/settings/page_registry.h"

int main() {
    using namespace modernime::settings;
    const auto pages = settingsPageDefinitions();
    assert(pages.size() == 6);
    assert(pages.front().id == SettingsPageId::Overview);
    assert(pages[1].id == SettingsPageId::Input);
    assert(pages.back().id == SettingsPageId::Diagnostics);

    const auto shortcut = searchSettings("快捷键");
    assert(!shortcut.empty());
    assert(shortcut.front().page == SettingsPageId::Input);
    assert(shortcut.front().target == "toggle-key");

    const auto alias = searchSettings("fcitx");
    assert(!alias.empty());
    assert(alias.front().page == SettingsPageId::Diagnostics);
    assert(searchSettings("   ").empty());
}
```

- [ ] **Step 2: Run the test target and verify it fails**

Run: `cmake --preset fcitx5-debug && cmake --build --preset fcitx5-debug --target modernime_page_registry_tests`

Expected: compilation fails because `modernime/settings/page_registry.h` does not exist.

- [ ] **Step 3: Implement the immutable registry and ranked local matching**

```cpp
enum class SettingsPageId {
    Overview,
    Input,
    Dictionary,
    Clipboard,
    Learning,
    Diagnostics,
};

struct SettingsPageDefinition final {
    SettingsPageId id;
    std::string_view name;
    std::string_view title;
    std::string_view subtitle;
    std::string_view group;
};

struct SettingsSearchEntry final {
    SettingsPageId page;
    std::string_view target;
    std::string_view title;
    std::string_view description;
    std::string_view keywords;
};

std::span<const SettingsPageDefinition> settingsPageDefinitions();
std::span<const SettingsSearchEntry> settingsSearchEntries();
std::vector<SettingsSearchEntry> searchSettings(std::string_view query);
```

Implement ranking as exact title match, title substring, then description/keyword substring. Fold ASCII only for English aliases; preserve Chinese UTF-8 byte sequences unchanged. Populate entries for every phase-one input setting and the principal actions on dictionary, clipboard, learning, and diagnostics pages.

Rename the current inline page array to `legacySettingsPageDefinitions()` and update `settings_window.cpp` to use that compatibility name until Task 10 removes it. This prevents an ODR/signature collision while keeping the current client functional during the migration.

- [ ] **Step 4: Run focused tests and the existing UI contract test**

Run: `cmake --build --preset fcitx5-debug --target modernime_page_registry_tests modernime_settings_ui_contract_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(page_registry|settings_ui_contract)' --output-on-failure`

Expected: both tests pass.

- [ ] **Step 5: Commit the registry**

```bash
git add tools/settings/include/modernime/settings/page_registry.h tools/settings/page_registry.cpp tools/settings/include/modernime/settings/settings_ui_contract.h tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/page_registry_test.cpp tests/settings/settings_ui_contract_test.cpp tests/CMakeLists.txt
git commit -m "新增设置页面注册与搜索索引"
```

### Task 2: Field-addressable validation and edit-session lifecycle

**Files:**
- Modify: `core/include/modernime/core/settings.h`
- Modify: `core/src/settings.cpp`
- Modify: `tools/settings/include/modernime/settings/settings_model.h`
- Modify: `tools/settings/settings_model.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tests/core/settings_test.cpp`
- Modify: `tests/settings/settings_window_model_test.cpp`

**Interfaces:**
- Consumes: `core::ModernIMESettings` and `core::SettingsStore`.
- Produces: `SettingsValidationIssue`, `SettingsValidationResult::issues`, `SettingsWindowModel::savedSettings()`, `editDefaults()`, `reloadRequired()`, and `markReloaded()`.

- [ ] **Step 1: Write failing validation and lifecycle assertions**

```cpp
auto invalid = modernime::core::defaultSettings();
invalid.toggleKey = "Ctrl Space";
const auto validation = modernime::core::validateSettings(invalid);
assert(!validation.valid);
assert(validation.issues.size() == 1);
assert(validation.issues.front().key == "input.toggle_key");

modernime::settings::SettingsWindowModel model(path);
model.editDefaults();
assert(model.dirty());
assert(!std::filesystem::exists(path));
std::string error;
assert(model.save(&error));
assert(model.reloadRequired());
model.markReloaded();
assert(!model.reloadRequired());
```

- [ ] **Step 2: Run the two model targets and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_tests modernime_settings_model_tests`

Expected: compilation fails because `issues`, `editDefaults`, `reloadRequired`, and `markReloaded` are not defined.

- [ ] **Step 3: Implement keyed issues and non-destructive defaults editing**

```cpp
struct SettingsValidationIssue final {
    std::string key;
    std::string message;
};

struct SettingsValidationResult final {
    bool valid = true;
    std::vector<SettingsValidationIssue> issues;
};

class SettingsWindowModel final {
public:
    const core::ModernIMESettings &settings() const { return edited_; }
    const core::ModernIMESettings &savedSettings() const { return loaded_; }
    void editDefaults();
    bool reloadRequired() const { return reloadRequired_; }
    void markReloaded() { reloadRequired_ = false; }
    // Existing save/reset-edit methods remain.
private:
    bool reloadRequired_ = false;
};
```

In `save()`, compute `changed = edited_ != loaded_` before persistence and set `reloadRequired_ = reloadRequired_ || changed` only after a successful save. Replace the current immediate-persistence `resetDefaults()` UI path with `editDefaults()`; keep `SettingsStore::reset()` available for non-UI callers.

Update every current validation consumer in `SettingsStore::save()`, `settings_window.cpp`, `settings_test.cpp`, and `settings_window_model_test.cpp` from `errors.front()` to `issues.front().message`. Use the issue key `input.toggle_key` or `clipboard.trigger` to attach the message to the matching entry control.

- [ ] **Step 4: Run core and settings-model tests**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_tests modernime_settings_model_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(settings|settings_model)$' --output-on-failure`

Expected: both tests pass, including failed-save preservation and reload-state assertions.

- [ ] **Step 5: Commit the lifecycle behavior**

```bash
git add core/include/modernime/core/settings.h core/src/settings.cpp tools/settings/include/modernime/settings/settings_model.h tools/settings/settings_model.cpp tools/settings/settings_window.cpp tests/core/settings_test.cpp tests/settings/settings_window_model_test.cpp
git commit -m "完善设置校验与编辑生命周期"
```

### Task 3: Shared GTK3 widgets and theme-safe styles

**Files:**
- Create: `tools/settings/include/modernime/settings/settings_widgets.h`
- Create: `tools/settings/settings_widgets.cpp`
- Create: `tests/settings/settings_widgets_contract_test.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: GTK3 only.
- Produces: `installSettingsStyles()`, `createPageShell()`, `createSectionCard()`, `createScrollablePage()`, `createSettingRow()`, `createStatusPill()`, `createEmptyState()`, and public style-class constants.

- [ ] **Step 1: Write the failing widget-contract test**

```cpp
#include "modernime/settings/settings_widgets.h"

int main() {
    using namespace modernime::settings;
    static_assert(kSettingsWindowClass == "modernime-settings");
    static_assert(kSettingsSidebarClass == "modernime-sidebar");
    static_assert(kSettingsSectionClass == "modernime-section");
    static_assert(kSettingsPrimaryButtonClass == "suggested-action");
    static_assert(kSettingsDangerButtonClass == "destructive-action");
    assert(settingsStyles().find("@theme_bg_color") != std::string_view::npos);
    assert(settingsStyles().find("linear-gradient") == std::string_view::npos);
}
```

- [ ] **Step 2: Run the widget-contract target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_widgets_contract_tests`

Expected: compilation fails because `settings_widgets.h` does not exist.

- [ ] **Step 3: Move shared builders and CSS out of `settings_window.cpp`**

```cpp
inline constexpr std::string_view kSettingsWindowClass = "modernime-settings";
inline constexpr std::string_view kSettingsSidebarClass = "modernime-sidebar";
inline constexpr std::string_view kSettingsSectionClass = "modernime-section";
inline constexpr std::string_view kSettingsPrimaryButtonClass = "suggested-action";
inline constexpr std::string_view kSettingsDangerButtonClass = "destructive-action";

void installSettingsStyles();
std::string_view settingsStyles();
GtkWidget *createPageShell(std::string_view title,
                           std::string_view subtitle);
GtkWidget *createSectionCard(std::string_view title,
                             std::string_view description);
GtkWidget *createScrollablePage(GtkWidget *page);
```

Use system theme colors, 12 px section radius, 16 px section padding, 24 px page margins, 12 px control gaps, and 20 px/600-weight page headings. Add accessible names and descriptions at factory call sites; factories must not invent labels that differ from visible text.

- [ ] **Step 4: Run contract tests and compile the client**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_widgets_contract_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R modernime_settings_widgets_contract --output-on-failure`

Expected: widget contract passes and the client links with no duplicate builder symbols.

- [ ] **Step 5: Commit shared UI foundations**

```bash
git add tools/settings/include/modernime/settings/settings_widgets.h tools/settings/settings_widgets.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/settings_widgets_contract_test.cpp tests/CMakeLists.txt
git commit -m "抽取设置客户端共享界面组件"
```

### Task 4: Consolidated input page

**Files:**
- Create: `tools/settings/include/modernime/settings/pages/input_page.h`
- Create: `tools/settings/pages/input_page.cpp`
- Create: `tests/settings/input_page_state_test.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SettingsWindowModel`, keyed validation issues, and shared widget factories.
- Produces: `InputPageState`, `deriveInputPageState(const SettingsWindowModel &)`, and `InputPage` with `widget()`, `refresh()`, and `focusTarget(std::string_view)`.

- [ ] **Step 1: Write the failing input-state test**

```cpp
modernime::settings::SettingsWindowModel model(path);
auto settings = model.settings();
settings.inputEnabled = false;
settings.toggleKey = "Ctrl Space";
model.setSettings(settings);

const auto state = modernime::settings::deriveInputPageState(model);
assert(!state.dependentControlsSensitive);
assert(!state.toggleKeyValid);
assert(state.toggleKeyMessage.find("只能包含") != std::string::npos);
assert(!state.canApply);
```

- [ ] **Step 2: Run the input-page state target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_input_page_state_tests`

Expected: compilation fails because the input-page state API is absent.

- [ ] **Step 3: Implement state derivation and the merged page**

```cpp
struct InputPageState final {
    bool dependentControlsSensitive = true;
    bool toggleKeyValid = true;
    std::string toggleKeyMessage;
    bool canApply = true;
};

class InputPage final {
public:
    InputPage(SettingsWindowModel &model,
              std::function<void()> changed);
    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);
};
```

Create four sections in this order: input status, shortcut, punctuation, candidate behavior. Reuse the exact existing settings fields and preserve all current signal behavior. Set target IDs `input-enabled`, `default-mode`, `toggle-key`, `punctuation`, `number-selection`, `arrow-navigation`, and `page-navigation` for search focus.

- [ ] **Step 4: Run state tests and compile the client**

Run: `cmake --build --preset fcitx5-debug --target modernime_input_page_state_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R modernime_input_page_state --output-on-failure`

Expected: state tests pass and all former basic/candidate controls compile from `input_page.cpp`.

- [ ] **Step 5: Commit the input page**

```bash
git add tools/settings/include/modernime/settings/pages/input_page.h tools/settings/pages/input_page.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/input_page_state_test.cpp tests/CMakeLists.txt
git commit -m "合并输入与候选设置页面"
```

### Task 5: Overview model and page

**Files:**
- Create: `tools/settings/include/modernime/settings/overview_model.h`
- Create: `tools/settings/overview_model.cpp`
- Create: `tools/settings/include/modernime/settings/pages/overview_page.h`
- Create: `tools/settings/pages/overview_page.cpp`
- Create: `tests/settings/overview_model_test.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `core::SettingsPaths`, `core::ModernIMESettings`, `DataController`, `ClipboardHistoryModel`, and `RuntimeStatus`.
- Produces: `OverviewSnapshot`, `OverviewNotice`, `collectOverviewSnapshot(...)`, and `OverviewPage::setSnapshot(...)`.

- [ ] **Step 1: Write the failing overview-model test**

```cpp
modernime::settings::RuntimeStatus runtime;
runtime.available = true;
runtime.running = true;
runtime.modernimeAvailable = true;
runtime.modernimeActive = true;

const auto snapshot = modernime::settings::collectOverviewSnapshot(
    paths, modernime::core::defaultSettings(), runtime);
assert(snapshot.runtimeSummary == "ModernIME 正在运行");
assert(snapshot.defaultMode == "中文");
assert(snapshot.dictionaryEntries == 2);
assert(snapshot.clipboardEntries == 1);
assert(snapshot.learningEntries == 1);
assert(snapshot.notices.empty());
```

Create real temporary dictionary, clipboard, and learning files with the existing public store APIs. Add a second case where paths cannot be read and assert a non-empty notice with `SettingsPageId::Diagnostics`.

- [ ] **Step 2: Run the overview-model target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_overview_model_tests`

Expected: compilation fails because `overview_model.h` does not exist.

- [ ] **Step 3: Implement snapshot collection and overview cards**

```cpp
struct OverviewNotice final {
    SettingsPageId destination;
    std::string message;
};

struct OverviewSnapshot final {
    std::string runtimeSummary;
    std::string defaultMode;
    std::string toggleKey;
    std::size_t dictionaryEntries = 0;
    std::size_t clipboardEntries = 0;
    std::size_t learningEntries = 0;
    std::vector<OverviewNotice> notices;
};

OverviewSnapshot collectOverviewSnapshot(
    const core::SettingsPaths &paths,
    const core::ModernIMESettings &settings,
    const RuntimeStatus &runtime);
```

`OverviewPage` starts in a loading state, accepts a finished snapshot on the main thread, and exposes destination callbacks for input, dictionary, clipboard, learning, and diagnostics. Collection runs in a `GTask`; no file or runtime probe executes inside the GTK completion callback.

- [ ] **Step 4: Run model tests and compile the page**

Run: `cmake --build --preset fcitx5-debug --target modernime_overview_model_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R modernime_overview_model --output-on-failure`

Expected: model tests pass and the overview page links.

- [ ] **Step 5: Commit overview functionality**

```bash
git add tools/settings/include/modernime/settings/overview_model.h tools/settings/overview_model.cpp tools/settings/include/modernime/settings/pages/overview_page.h tools/settings/pages/overview_page.cpp tools/settings/CMakeLists.txt tests/settings/overview_model_test.cpp tests/CMakeLists.txt
git commit -m "新增设置客户端概览页面"
```

### Task 6: Extract the clipboard page without behavior changes

**Files:**
- Create: `tools/settings/include/modernime/settings/pages/clipboard_page.h`
- Create: `tools/settings/pages/clipboard_page.cpp`
- Create: `tests/settings/clipboard_page_contract_test.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SettingsWindowModel`, `ClipboardHistoryModel`, shared widgets, and a global-message callback.
- Produces: `ClipboardPage` with `widget()`, `refresh(bool notify)`, `refreshSettings()`, and `focusTarget(std::string_view)`.

- [ ] **Step 1: Write the failing clipboard-page contract test**

```cpp
#include "modernime/settings/pages/clipboard_page.h"
#include <type_traits>

int main() {
    using modernime::settings::ClipboardPage;
    using modernime::settings::SettingsPageId;
    static_assert(ClipboardPage::pageId == SettingsPageId::Clipboard);
    static_assert(!std::is_copy_constructible_v<ClipboardPage>);
}
```

- [ ] **Step 2: Run the contract target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_clipboard_page_contract_tests`

Expected: compilation fails because `clipboard_page.h` does not exist.

- [ ] **Step 3: Move clipboard widgets and callbacks into `ClipboardPage`**

```cpp
class ClipboardPage final {
public:
    static constexpr auto pageId = SettingsPageId::Clipboard;
    ClipboardPage(SettingsWindowModel &settings,
                  std::filesystem::path historyPath,
                  std::function<void()> settingsChanged,
                  std::function<void(std::string)> notify);
    GtkWidget *widget() const;
    void refresh(bool notify);
    void refreshSettings();
    bool focusTarget(std::string_view target);
};
```

Preserve enable/trigger editing, automatic refresh on navigation, copy, single deletion, clear confirmation, empty state, and current atomic persistence. Assign search targets `clipboard-enabled`, `clipboard-trigger`, and `clipboard-history`. Retain `modernime_clipboard_history_model_tests` as the behavioral regression suite.

- [ ] **Step 4: Run clipboard and client tests after extraction**

Run: `cmake --build --preset fcitx5-debug --target modernime_clipboard_page_contract_tests modernime_clipboard_history_model_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(clipboard_page_contract|clipboard_history_model|settings_ui_contract)' --output-on-failure`

Expected: tests pass and clipboard callback symbols no longer exist in `settings_window.cpp`.

- [ ] **Step 5: Commit the clipboard page extraction**

```bash
git add tools/settings/include/modernime/settings/pages/clipboard_page.h tools/settings/pages/clipboard_page.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/clipboard_page_contract_test.cpp tests/CMakeLists.txt
git commit -m "拆分设置客户端剪贴板页面"
```

### Task 7: Extract the learning page without behavior changes

**Files:**
- Create: `tools/settings/include/modernime/settings/pages/learning_page.h`
- Create: `tools/settings/pages/learning_page.cpp`
- Create: `tests/settings/learning_page_contract_test.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SettingsWindowModel`, `DataController`, shared widgets, settings-change callback, and global-message callback.
- Produces: `LearningPage` with `widget()`, `refresh(bool notify)`, `refreshSettings()`, and `focusTarget(std::string_view)`.

- [ ] **Step 1: Write the failing learning-page contract test**

```cpp
#include "modernime/settings/pages/learning_page.h"
#include <type_traits>

int main() {
    using modernime::settings::LearningPage;
    using modernime::settings::SettingsPageId;
    static_assert(LearningPage::pageId == SettingsPageId::Learning);
    static_assert(!std::is_copy_constructible_v<LearningPage>);
}
```

- [ ] **Step 2: Run the contract target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_learning_page_contract_tests`

Expected: compilation fails because `learning_page.h` does not exist.

- [ ] **Step 3: Move learning widgets and callbacks into `LearningPage`**

```cpp
class LearningPage final {
public:
    static constexpr auto pageId = SettingsPageId::Learning;
    LearningPage(SettingsWindowModel &settings,
                 std::filesystem::path learningPath,
                 std::function<void()> settingsChanged,
                 std::function<void(std::string)> notify);
    GtkWidget *widget() const;
    void refresh(bool notify);
    void refreshSettings();
    bool focusTarget(std::string_view target);
};
```

Preserve learning/context switches, database path, entry count, refresh, backup-before-clear confirmation, failure text, and dependent sensitivity. Assign targets `learning-enabled`, `context-learning`, and `learning-data`. Retain `modernime_data_controller_tests` as the backup/clear behavioral suite.

- [ ] **Step 4: Run learning/data tests and compile the client**

Run: `cmake --build --preset fcitx5-debug --target modernime_learning_page_contract_tests modernime_data_controller_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(learning_page_contract|data_controller)' --output-on-failure`

Expected: tests pass and learning widget ownership resides in `LearningPage`.

- [ ] **Step 5: Commit the learning page extraction**

```bash
git add tools/settings/include/modernime/settings/pages/learning_page.h tools/settings/pages/learning_page.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/learning_page_contract_test.cpp tests/CMakeLists.txt
git commit -m "拆分设置客户端智能学习页面"
```

### Task 8: Extract the dictionary page without behavior changes

**Files:**
- Create: `tools/settings/include/modernime/settings/pages/dictionary_page.h`
- Create: `tools/settings/pages/dictionary_page.cpp`
- Create: `tests/settings/dictionary_page_contract_test.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataController`, shared widgets, and global-message callback.
- Produces: `DictionaryPage` with `widget()`, `refresh()`, and `focusTarget(std::string_view)`.

- [ ] **Step 1: Write the failing dictionary-page contract test**

```cpp
#include "modernime/settings/pages/dictionary_page.h"
#include <type_traits>

int main() {
    using modernime::settings::DictionaryPage;
    using modernime::settings::SettingsPageId;
    static_assert(DictionaryPage::pageId == SettingsPageId::Dictionary);
    static_assert(!std::is_copy_constructible_v<DictionaryPage>);
}
```

- [ ] **Step 2: Run the contract target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_dictionary_page_contract_tests`

Expected: compilation fails because `dictionary_page.h` does not exist.

- [ ] **Step 3: Move dictionary widgets and callbacks into `DictionaryPage`**

```cpp
class DictionaryPage final {
public:
    static constexpr auto pageId = SettingsPageId::Dictionary;
    DictionaryPage(std::filesystem::path dictionaryPath,
                   std::function<void(std::string)> notify);
    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);
};
```

Preserve search, add, edit, delete, replacement confirmation on import, export, selection sensitivity, empty state, input checks, and current atomic-save behavior. Assign search targets `dictionary-search`, `dictionary-add`, and `dictionary-import-export`. Retain `modernime_data_controller_tests` as the import/save behavioral suite.

- [ ] **Step 4: Run data tests and compile the client**

Run: `cmake --build --preset fcitx5-debug --target modernime_dictionary_page_contract_tests modernime_data_controller_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(dictionary_page_contract|data_controller)' --output-on-failure`

Expected: tests pass and dictionary callbacks no longer live in `settings_window.cpp`.

- [ ] **Step 5: Commit the dictionary page extraction**

```bash
git add tools/settings/include/modernime/settings/pages/dictionary_page.h tools/settings/pages/dictionary_page.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/dictionary_page_contract_test.cpp tests/CMakeLists.txt
git commit -m "拆分设置客户端用户词典页面"
```

### Task 9: Extract and relabel runtime diagnostics

**Files:**
- Create: `tools/settings/include/modernime/settings/pages/diagnostics_page.h`
- Create: `tools/settings/pages/diagnostics_page.cpp`
- Create: `tests/settings/diagnostics_page_contract_test.cpp`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `RuntimeController`, current environment/path discovery helpers, shared widgets, and global-message callback.
- Produces: `DiagnosticsPage` with `widget()`, `refresh()`, `reload()`, and `focusTarget(std::string_view)`.

- [ ] **Step 1: Write the failing diagnostics-page contract test**

```cpp
#include "modernime/settings/pages/diagnostics_page.h"
#include <type_traits>

int main() {
    using modernime::settings::DiagnosticsPage;
    using modernime::settings::SettingsPageId;
    static_assert(DiagnosticsPage::pageId == SettingsPageId::Diagnostics);
    static_assert(!std::is_copy_constructible_v<DiagnosticsPage>);
}
```

- [ ] **Step 2: Run the contract target and verify failure**

Run: `cmake --build --preset fcitx5-debug --target modernime_diagnostics_page_contract_tests`

Expected: compilation fails because `diagnostics_page.h` does not exist.

- [ ] **Step 3: Move status widgets and `GTask` callbacks into `DiagnosticsPage`**

```cpp
class DiagnosticsPage final {
public:
    static constexpr auto pageId = SettingsPageId::Diagnostics;
    DiagnosticsPage(std::filesystem::path fcitx,
                    std::filesystem::path remote,
                    Environment environment,
                    std::function<void(std::string)> notify);
    GtkWidget *widget() const;
    void refresh();
    void reload();
    bool focusTarget(std::string_view target);
};
```

Preserve asynchronous probe/reload, disable both buttons during work, show separate availability/service/current-input/ModernIME rows, and restore retry controls after every result. Use the visible page title `系统与诊断`; do not add filesystem diagnostics until phase three. Retain `modernime_runtime_controller_tests`, including unavailable, timeout, no-input-context, and reload failure cases.

- [ ] **Step 4: Run runtime tests and compile the client**

Run: `cmake --build --preset fcitx5-debug --target modernime_diagnostics_page_contract_tests modernime_runtime_controller_tests modernime-settings && ctest --test-dir build/fcitx5-debug -R 'modernime_(diagnostics_page_contract|runtime_controller)' --output-on-failure`

Expected: tests pass and all runtime GTK task state is owned by `DiagnosticsPage`.

- [ ] **Step 5: Commit diagnostics extraction**

```bash
git add tools/settings/include/modernime/settings/pages/diagnostics_page.h tools/settings/pages/diagnostics_page.cpp tools/settings/settings_window.cpp tools/settings/CMakeLists.txt tests/settings/diagnostics_page_contract_test.cpp tests/CMakeLists.txt
git commit -m "拆分设置客户端系统诊断页面"
```

### Task 10: Settings shell, navigation, search focus, and save feedback

**Files:**
- Create: `tools/settings/include/modernime/settings/settings_shell.h`
- Create: `tools/settings/settings_shell.cpp`
- Modify: `tools/settings/include/modernime/settings/settings_window.h`
- Modify: `tools/settings/settings_window.cpp`
- Modify: `tools/settings/include/modernime/settings/settings_ui_contract.h`
- Modify: `tools/settings/CMakeLists.txt`
- Modify: `tests/settings/settings_ui_contract_test.cpp`
- Modify: `tests/settings/settings_window_model_test.cpp`

**Interfaces:**
- Consumes: every page class, page registry/search, `SettingsWindowModel`, and shared widgets.
- Produces: `SettingsShell`, page navigation by `SettingsPageId`, search-result focus routing, and the final phase-one window behavior.

- [ ] **Step 1: Replace the old UI contract assertions with the approved phase-one contract**

```cpp
const auto pages = modernime::settings::settingsPageDefinitions();
assert(pages.size() == 6);
assert(pages[0].title == "概览");
assert(pages[1].title == "输入体验");
assert(pages[2].title == "个人词典");
assert(pages[3].title == "剪贴板");
assert(pages[4].title == "智能学习");
assert(pages[5].title == "系统与诊断");

const auto actions = modernime::settings::settingsActionLabels();
assert((actions == std::array<std::string_view, 3>{
    "恢复修改", "应用", "保存并关闭"}));
```

Add a model assertion that failed save keeps edits and does not set `reloadRequired()`.

- [ ] **Step 2: Run UI/model tests and verify the contract is red**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_ui_contract_tests modernime_settings_model_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(settings_ui_contract|settings_model)' --output-on-failure`

Expected: UI contract fails against the old page/action definitions.

- [ ] **Step 3: Implement `SettingsShell` and reduce `SettingsWindow` to coordination**

```cpp
class SettingsShell final {
public:
    SettingsShell(GtkApplication *application,
                  core::SettingsPaths paths);
    GtkWidget *window() const;
    void present();
    void show(SettingsPageId page,
              std::string_view target = {});
    void presentError(std::string_view message);
};
```

Build grouped navigation with overview first, a search entry above navigation, a `GtkPopover` result list, the page stack, and a fixed bottom bar. Selecting a search result must call `show(result.page, result.target)`. Keep window default size 860×620 and define a tested minimum size of 720×520.

Save behavior is: validate, disable action buttons, save, restore buttons, show failure without discarding the draft, or show “设置已保存，需要重新加载 ModernIME”. `应用` keeps the window open; `保存并关闭` closes only after success. Close with dirty state offers `继续编辑`, `放弃修改`, and `保存并关闭`. `恢复默认` lives in the input page overflow/menu and only edits the draft.

- [ ] **Step 4: Run focused tests, compile, and run the complete suite**

Run: `cmake --build --preset fcitx5-debug --target modernime-settings modernime_settings_ui_contract_tests modernime_settings_model_tests && ctest --preset fcitx5-debug`

Expected: all CTest cases pass with zero failures.

- [ ] **Step 5: Commit the shell integration**

```bash
git add tools/settings/include/modernime/settings/settings_shell.h tools/settings/settings_shell.cpp tools/settings/include/modernime/settings/settings_window.h tools/settings/settings_window.cpp tools/settings/include/modernime/settings/settings_ui_contract.h tools/settings/CMakeLists.txt tests/settings/settings_ui_contract_test.cpp tests/settings/settings_window_model_test.cpp
git commit -m "重构设置客户端导航与保存交互"
```

### Task 11: Accessibility, installation, documentation, and final verification

**Files:**
- Modify: `tools/settings/settings_widgets.cpp`
- Modify: `tools/settings/pages/overview_page.cpp`
- Modify: `tools/settings/pages/input_page.cpp`
- Modify: `tools/settings/pages/clipboard_page.cpp`
- Modify: `tools/settings/pages/learning_page.cpp`
- Modify: `tools/settings/pages/dictionary_page.cpp`
- Modify: `tools/settings/pages/diagnostics_page.cpp`
- Modify: `tests/settings/settings_ui_contract_test.cpp`
- Modify: `tests/settings/settings_install_test.sh`
- Modify: `config/desktop/modernime-settings.desktop`
- Modify: `README.md`

**Interfaces:**
- Consumes: the completed phase-one client.
- Produces: a keyboard-accessible, theme-safe, installed application with accurate documentation.

- [ ] **Step 1: Add final contract and installation assertions**

```cpp
const auto pageClasses = modernime::settings::settingsPageSurfaceStyleClasses();
assert(std::ranges::find(pageClasses, "modernime-page-scroller") !=
       pageClasses.end());
assert(std::ranges::find(pageClasses, "modernime-page-surface") !=
       pageClasses.end());
```

In `settings_install_test.sh`, retain the installed binary/desktop checks and add `grep -Fqx 'Comment=配置 ModernIME 输入体验与个人数据'` against the installed desktop file. Update `config/desktop/modernime-settings.desktop` from `Comment=配置 ModernIME 输入法` to that exact new comment.

- [ ] **Step 2: Run contract and install tests to expose missing copy or metadata**

Run: `cmake --build --preset fcitx5-debug --target modernime_settings_ui_contract_tests && ctest --test-dir build/fcitx5-debug -R 'modernime_(settings_ui_contract|settings_install)' --output-on-failure`

Expected: the new assertion fails until all metadata and style contracts match.

- [ ] **Step 3: Complete accessibility and documentation details**

For every interactive widget, set an accessible name matching its visible label and a description explaining the result. Establish deterministic Tab order within each page; give the search field `Ctrl+F`; give Apply `Ctrl+Enter`; let Escape close the search popover without discarding edits. Ensure disabled child settings remain readable. Update README page descriptions, settings search behavior, save/reload lifecycle, and the statement that phase one performs no network requests.

- [ ] **Step 4: Run full build, tests, install, and smoke checks**

Run: `cmake --preset fcitx5-debug`

Expected: configure succeeds.

Run: `cmake --build --preset fcitx5-debug`

Expected: all targets build successfully with warnings treated according to the existing project policy.

Run: `ctest --preset fcitx5-debug`

Expected: all tests pass with zero failures.

Run: `cmake --install build/fcitx5-debug --prefix /tmp/modernime-settings-phase1-install`

Expected: `modernime-settings` and its desktop file install under the temporary prefix.

Run: `/tmp/modernime-settings-phase1-install/bin/modernime-settings --version`

Expected: output starts with `ModernIME settings 0.1.0` and exits successfully.

- [ ] **Step 5: Perform manual GTK checks in a graphical session**

Launch the installed client with temporary `HOME`, `XDG_CONFIG_HOME`, and `XDG_DATA_HOME`. Verify overview loading, all six pages, search-to-focus, 720×520 minimum layout, light/dark system themes, pure-keyboard navigation, dirty-close choices, invalid shortcut feedback, save failure preservation, successful save, and runtime reload retry. Record the checklist result in the commit body; do not mark the task complete if a required graphical check cannot be performed.

- [ ] **Step 6: Commit phase-one completion**

```bash
git add tools/settings/settings_widgets.cpp tools/settings/pages/overview_page.cpp tools/settings/pages/input_page.cpp tools/settings/pages/clipboard_page.cpp tools/settings/pages/learning_page.cpp tools/settings/pages/dictionary_page.cpp tools/settings/pages/diagnostics_page.cpp tests/settings/settings_ui_contract_test.cpp tests/settings/settings_install_test.sh config/desktop/modernime-settings.desktop README.md
git commit -m "完成设置客户端基础体验重构"
```

---

## Phase-One Acceptance Checklist

- The client opens on Overview and shows runtime, default mode, shortcut, dictionary count, clipboard count, learning count, and actionable notices.
- Search is local-only and routes each result to the correct page and control.
- Input and candidate behavior settings live on one page and expose field-level shortcut errors.
- Existing dictionary, clipboard, learning, and runtime operations remain functional after extraction.
- Save failure preserves the draft; successful changes visibly require reload until runtime reload succeeds.
- No network page, library, request, background timer, or unavailable action is present.
- GTK remains responsive during overview collection and runtime operations.
- Keyboard navigation, system light/dark themes, empty states, failure states, and minimum window size pass manual verification.
- Full configure, build, CTest, temporary-prefix install, and `--version` smoke checks pass.

## Later Plans

After phase one is accepted, create separate implementation plans from the same design spec for:

1. Phase two: dictionary batching/conflicts, clipboard pin/privacy/capacity, learning search/selective deletion/restore, and unified backup/restore.
2. Phase three: local diagnostics/report redaction and, only after a service-specific design and threat review, explicit opt-in networking.
