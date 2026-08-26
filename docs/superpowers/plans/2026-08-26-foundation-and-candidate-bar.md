# ModernIME Foundation and Candidate Bar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first usable ModernIME milestone: a clean Fcitx5 input-method addon with stable basic pinyin input and a candidate bar matching the supplied reference image.

**Architecture:** Keep the preserved algorithms behind a Fcitx5-independent core boundary. Add a thin Fcitx5 adapter for input-context state and candidate selection, and a separate UI addon for the ModernIME candidate bar. The first milestone uses a deterministic candidate provider so key handling and UI can be validated before the LibIME algorithm migration.

**Tech Stack:** C++20, CMake, Ninja, CTest, GoogleTest-free self-contained C++ test executables, Fcitx5 5.1.7 public addon APIs, LibIME pinyin APIs when development packages are available, Cairo/Pango or the public Fcitx5 UI drawing stack selected from the installed UI API, POSIX shell install scripts.

**Spec:** `docs/superpowers/specs/2026-08-26-modernime-fcitx5-design.md`

## Global Constraints

- Do not modify Fcitx5 itself.
- Do not delete or overwrite other input methods, Fcitx5 official files, or system directories.
- Development must not execute `sudo make install` or `sudo ninja install`.
- All installed files must be produced by `install.sh` and removed by `uninstall.sh`.
- Every independent feature or algorithm update gets its own `git commit`.
- Every stage is compiled, tested, and verified before the next stage.
- The first runtime target is Wayland with X11 compatibility through the same Fcitx5 public interfaces.
- The supplied candidate-bar image is the only visual reference; no extra UI elements may be introduced.

## File Map

The old `include/` and `src/` tree is not used as the new runtime layout. Preserved algorithms will be moved behind the new `core/` boundary only after the first input loop is working.

```text
CMakeLists.txt
CMakePresets.json
.gitignore
cmake/
  ModernIMEWarnings.cmake
  ModernIMEInstall.cmake
core/
  include/modernime/core/
  src/
adapter/fcitx5/
  include/modernime/fcitx5/
  src/
ui/fcitx5/
  include/modernime/ui/
  src/
config/
  modernime.conf.in
  modernime-addon.conf.in
  modernime-inputmethod.conf.in
tests/
  core/
  adapter/
  ui/
scripts/
  install.sh
  uninstall.sh
assets/reference/
  candidate-bar.png
docs/
  superpowers/specs/
  superpowers/plans/
```

### Task 1: Build and test foundation

**Files:**
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `.gitignore`
- Create: `cmake/ModernIMEWarnings.cmake`
- Create: `core/CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/core/foundation_test.cpp`

**Interfaces:**
- Produces a `modernime_core` library target, a `modernime_core_tests` executable, and a CTest registration.
- `cmake -S . -B build -G "Unix Makefiles" -DMODERNIME_BUILD_FCITX5=OFF` must configure without Fcitx5 development headers. Ninja remains an optional generator when installed.

- [ ] **Step 1: Write the failing test**

Create a self-contained test executable with a real assertion that includes the future public core include path and verifies the initial test harness exits successfully only after a named test is registered:

```cpp
#include <iostream>

int main() {
    std::cout << "foundation test placeholder is intentionally failing\n";
    return 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake -S . -B build -G "Unix Makefiles" -DMODERNIME_BUILD_FCITX5=OFF
cmake --build build --target modernime_core_tests
ctest --test-dir build --output-on-failure
```

Expected: the test executable is built and CTest reports one failed test with exit code 1.

- [ ] **Step 3: Write minimal implementation**

Replace the intentional failure with a tiny assertion helper and a passing `foundation_compiles` test. Configure C++20, `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, and register the executable with CTest. Add `build/`, `out/`, generated install manifests, and editor files to `.gitignore`.

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmake --build build --clean-first
ctest --test-dir build --output-on-failure
```

Expected: CTest reports 1/1 passed and the compiler emits no warnings.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt CMakePresets.json .gitignore cmake tests
git commit -m "build: add clean CMake and CTest foundation"
```

### Task 2: Input state and candidate display model

**Files:**
- Create: `core/include/modernime/core/input_state.h`
- Create: `core/src/input_state.cpp`
- Create: `core/include/modernime/core/candidate_model.h`
- Create: `core/src/candidate_model.cpp`
- Create: `tests/core/input_state_test.cpp`
- Modify: `core/CMakeLists.txt`

**Interfaces:**
- `modernime::core::InputState::append(std::string_view)`, `eraseLast()`, `clear()`, `text()`, and `generation()`.
- `modernime::core::CandidateItem { std::string text; std::string fullPinyin; std::size_t sourceIndex; }`.
- `modernime::core::CandidatePage { std::string preedit; std::vector<CandidateItem> items; std::size_t cursor; std::uint64_t generation; }`.
- `CandidatePage::select(std::size_t)` returns `bool` and rejects out-of-range indexes without changing state.

- [ ] **Step 1: Write the failing tests**

Add tests for: append increments generation, backspace removes one ASCII input character, clear removes input and candidates, and selecting an invalid candidate returns false while a valid selection returns true.

```cpp
TEST_ASSERT(state.append("hai"));
TEST_ASSERT(state.text() == "hai");
TEST_ASSERT(state.generation() == 3);
TEST_ASSERT(state.eraseLast());
TEST_ASSERT(state.text() == "ha");
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target modernime_core_tests
ctest --test-dir build -R input_state --output-on-failure
```

Expected: compilation fails because `InputState` and `CandidatePage` do not exist.

- [ ] **Step 3: Write minimal implementation**

Implement only ASCII composition state for this milestone. Reject empty appends, make each appended byte advance generation, make `eraseLast()` a no-op failure on empty input, and keep candidate selection bounded. Do not import the old algorithm files yet.

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmake --build build --clean-first
ctest --test-dir build -R input_state --output-on-failure
```

Expected: all input-state and candidate-model assertions pass with no warnings.

- [ ] **Step 5: Commit**

```bash
git add core tests/core/input_state_test.cpp
git commit -m "feat: add independent input and candidate state"
```

### Task 3: Fcitx5 engine lifecycle and basic key handling

**Files:**
- Create: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Create: `adapter/fcitx5/src/engine.cpp`
- Create: `adapter/fcitx5/src/addon.cpp`
- Create: `adapter/fcitx5/src/inputmethod.cpp`
- Create: `tests/adapter/engine_state_test.cpp`
- Modify: `CMakeLists.txt`
- Create: `adapter/fcitx5/CMakeLists.txt`
- Create: `config/modernime-addon.conf.in`
- Create: `config/modernime-inputmethod.conf.in`

**Interfaces:**
- `ModernIMEEngine` owns a `FactoryFor<ModernIMEState>` and exposes the Fcitx5 callbacks required by the installed 5.1.7 headers: activate, deactivate, reset, and key event handling.
- `ModernIMEState` owns one `core::InputState`, one `core::CandidatePage`, an `active` flag, and the last published generation.
- The addon entry point registers addon name `modernime`; the input-method descriptor registers unique name `modernime`, language `zh_CN`, and label `拼`.
- The adapter must call `inputContext->inputPanel().setPreedit(...)`, `setCandidateList(...)`, and `inputContext->updateUserInterface(UserInterfaceComponent::InputPanel)` only from the Fcitx5 event thread.

- [ ] **Step 1: Write the failing tests**

Write adapter tests against a small fake `EngineHost` interface, not Fcitx5 internals. The fake host records `commitString`, `resetInputPanel`, and `publishPage` calls. Cover:

```cpp
TEST_ASSERT(host.type("hai"));
TEST_ASSERT(host.page().preedit == "hai");
TEST_ASSERT(host.type("\\b"));
TEST_ASSERT(host.page().preedit == "ha");
TEST_ASSERT(host.escape());
TEST_ASSERT(host.page().preedit.empty());
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target modernime_adapter_tests
ctest --test-dir build -R engine_state --output-on-failure
```

Expected: compilation fails because the adapter state and host contract do not exist.

- [ ] **Step 3: Write minimal implementation**

Implement only these keys: ASCII letters append; Backspace erases; Escape resets without committing; Enter commits the selected candidate when one is selected, otherwise commits the raw preedit; number keys 1–9 commit that candidate; space commits the first candidate; a configured toggle key switches the engine active state without changing committed text. Use a deterministic placeholder candidate provider that returns the supplied sample words for `hail` and a raw-input fallback for other strings.

Do not add learning, network access, fuzzy matching, or long-sentence behavior.

- [ ] **Step 4: Run test to verify it passes**

After installing or exposing the Fcitx5 development headers, run:

```bash
cmake -S . -B build -G "Unix Makefiles" -DMODERNIME_BUILD_FCITX5=ON
cmake --build build --clean-first
ctest --test-dir build --output-on-failure
```

Expected: core and adapter tests pass; the addon and input-method shared library targets link against Fcitx5 public libraries only.

- [ ] **Step 5: Commit**

```bash
git add adapter config tests/adapter CMakeLists.txt
git commit -m "feat: add stable Fcitx5 input engine loop"
```

### Task 4: Candidate-bar layout model and reference metrics

**Files:**
- Create: `ui/fcitx5/include/modernime/ui/candidate_bar_layout.h`
- Create: `ui/fcitx5/src/candidate_bar_layout.cpp`
- Create: `tests/ui/candidate_bar_layout_test.cpp`
- Create: `assets/reference/candidate-bar.png`
- Create: `ui/fcitx5/CMakeLists.txt`

**Interfaces:**
- `CandidateBarMetrics` stores panel width/height, panel radius, border width, shadow radius/opacity, horizontal padding, candidate gap, selected-pill radius, font family, font size, and font weight.
- `CandidateBarLayout::measure(const CandidatePage &, const CandidateBarMetrics &)` returns panel bounds, preedit baseline, candidate baseline, and one rectangle per visible candidate.
- `CandidateBarLayout::visibleItems()` caps the display at nine items and never creates a rectangle for a missing candidate.

- [ ] **Step 1: Write the failing tests**

Use the original 2172×724 reference dimensions and assert the initial measured relationships: preedit is above the panel, the panel is a single horizontal row, the first selected rectangle contains its candidate text, and at most nine candidate rectangles are returned. Add a golden metric assertion for the initial panel and selected-pill bounds after measuring the supplied image.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target modernime_ui_tests
ctest --test-dir build -R candidate_bar_layout --output-on-failure
```

Expected: compilation fails because the layout model does not exist.

- [ ] **Step 3: Write minimal implementation**

Store the image as a test/reference asset without modifying it. Implement a deterministic layout using explicit metrics, not theme defaults. Keep all geometry in logical pixels and apply the display scale exactly once at the UI boundary. Do not add icons, status text, arrows, or extra rows.

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmake --build build --clean-first
ctest --test-dir build -R candidate_bar_layout --output-on-failure
```

Expected: geometry assertions pass and the test records the fixed reference metrics used by the renderer.

- [ ] **Step 5: Commit**

```bash
git add ui tests/ui assets/reference/candidate-bar.png
git commit -m "feat: add candidate bar layout model"
```

### Task 5: Custom Fcitx5 UI addon renderer

**Files:**
- Create: `ui/fcitx5/src/ui_addon.cpp`
- Create: `ui/fcitx5/src/candidate_bar_renderer.cpp`
- Create: `ui/fcitx5/src/candidate_bar_renderer.h`
- Create: `tests/ui/candidate_bar_renderer_test.cpp`
- Modify: `ui/fcitx5/CMakeLists.txt`
- Modify: `config/modernime-addon.conf.in`

**Interfaces:**
- The UI addon receives Fcitx5 input-panel updates and renders only the active ModernIME input method using the layout model from Task 4.
- `CandidateBarRenderer::render(RenderSurface &, const CandidateBarLayout &, const RenderStyle &)` draws, in order: shadow, translucent white panel, border, selected blue pill, preedit text, and candidate text.
- For a non-ModernIME input method, the addon delegates to the configured/default UI path or leaves the existing UI untouched; it must not replace other input methods’ candidate content.

- [ ] **Step 1: Write the failing tests**

Create a recording `RenderSurface` that captures rounded rectangles, fills, strokes, text strings, baseline positions, and colors. Assert that the render order contains exactly one panel, one selected pill, one preedit string, and one string for each visible candidate; assert that no extra decorative element is emitted.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build --target modernime_ui_tests
ctest --test-dir build -R candidate_bar_renderer --output-on-failure
```

Expected: compilation fails because the renderer and recording surface do not exist.

- [ ] **Step 3: Write minimal implementation**

Implement the renderer against the installed public Fcitx5 UI API and the chosen text/rendering backend. Keep painting on the UI/event thread, avoid disk I/O and locks in the paint path, and expose a safe no-op fallback if the custom UI addon cannot initialize.

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
cmake --build build --clean-first
ctest --test-dir build --output-on-failure
```

Then launch a disposable user-scoped Fcitx5 process with the development data paths and capture a screenshot at the reference scale. Compare the screenshot against `assets/reference/candidate-bar.png`; record protocol, scale, font, resolution, compositor, and any unavoidable rasterization difference.

- [ ] **Step 5: Commit**

```bash
git add ui config tests/ui
git commit -m "feat: render ModernIME candidate bar UI"
```

### Task 6: User-scoped install and uninstall tracking

**Files:**
- Create: `scripts/install.sh`
- Create: `scripts/uninstall.sh`
- Create: `cmake/ModernIMEInstall.cmake`
- Create: `packaging/manifest-format.md`
- Create: `tests/install/manifest_test.sh`

**Interfaces:**
- `scripts/install.sh --prefix "$HOME/.local" --config-home "$HOME/.config"` installs only the built addon, input-method descriptor, UI resources, and an install manifest under the chosen user prefix.
- `scripts/uninstall.sh --manifest <path>` removes only paths recorded in that manifest and never uses a recursive wildcard outside ModernIME-owned directories.

- [ ] **Step 1: Write the failing test**

Create a shell test using `mktemp -d` as a fake prefix. It should expect the installer to create a manifest listing every installed file and the uninstaller to remove those files while preserving an unrelated sentinel file in the same parent directory.

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
bash tests/install/manifest_test.sh
```

Expected: FAIL because the scripts do not exist.

- [ ] **Step 3: Write minimal implementation**

Implement strict shell options, explicit path lists, existing-file checks, atomic manifest writes, and no `sudo`. The default prefix is `${XDG_DATA_HOME:-$HOME/.local}` and the default config home is `${XDG_CONFIG_HOME:-$HOME/.config}`. Installation must not touch `/usr` or `/usr/local`.

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
bash tests/install/manifest_test.sh
```

Expected: PASS, including the sentinel-preservation assertion.

- [ ] **Step 5: Commit**

```bash
git add scripts cmake/ModernIMEInstall.cmake packaging tests/install
git commit -m "build: add tracked user-scoped install and uninstall"
```

## Plan Self-Review

- The first milestone covers the spec’s clean build, stable basic input, candidate list, UI separation, screenshot comparison, user-scoped installation, and per-feature commit requirements.
- Core algorithm migration, SQLite learning, privacy filtering, fuzzy matching, spelling correction, and long-sentence input are deliberately excluded from this first plan and will receive separate plans after the basic input/UI milestone is verified.
- No task uses a system-wide install or modifies Fcitx5 itself.
- Every production-code task starts with a failing test and ends with a focused commit.
- The only open environmental prerequisite is the distribution’s Fcitx5 development headers/libraries; the build configuration must report a clear configure error if they are unavailable.
