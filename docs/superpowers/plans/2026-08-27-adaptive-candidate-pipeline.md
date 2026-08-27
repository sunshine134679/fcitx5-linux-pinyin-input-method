# ModernIME Adaptive Candidate Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add offline user learning, local professional vocabulary, adaptive candidate ranking, reliable nine-item paging, learned-word deletion, and bounded surrounding-text ranking to ModernIME without changing the existing candidate-bar dimensions or visual styling.

**Architecture:** Keep LibIME responsible for pinyin segmentation, dictionary traversal, language-model decoding, and N-best generation. Add a small core learning layer backed by SQLite and a pinyin-side dictionary loader; pass immutable learning/context snapshots into the existing ranker. Keep all candidates in the controller/provider, let Fcitx5 paginate at nine items, and keep UI rendering unchanged.

**Tech Stack:** C++20, CMake, LibIME Core/Pinyin, Fcitx5 Core/Utils, SQLite3, existing CTest executables.

**Spec:** `docs/superpowers/specs/2026-08-27-adaptive-candidate-pipeline-design.md`

## Global Constraints

- Do not change the existing candidate-bar fixed dimensions, position, fonts, corner radii, or spacing.
- Do not add cloud candidates, network search, remote models, or neural models.
- Record only explicit candidate selections and deletions; do not record every keystroke.
- Keep LibIME as the pinyin decoder and use its native order as the primary ranking baseline.
- Candidate lists contain all results internally and display nine candidates per Fcitx5 page.
- Every task must follow failing test → minimal implementation → full tests → one independent git commit.
- Do not stage or modify the pre-existing untracked `include/` and `src/` directories in the main checkout.

---

### Task 1: Persistent user learning and ranker integration

**Files:**
- Create: `core/include/modernime/core/learning_snapshot.h`
- Create: `core/include/modernime/core/learning_store.h`
- Create: `core/include/modernime/core/learning_writer.h`
- Create: `core/src/learning_snapshot.cpp`
- Create: `core/src/learning_store.cpp`
- Create: `core/src/learning_writer.cpp`
- Modify: `core/include/modernime/core/candidate_ranker.h`
- Modify: `core/src/candidate_ranker.cpp`
- Modify: `core/CMakeLists.txt`
- Modify: `algorithm/pinyin/include/modernime/pinyin/candidate_pipeline.h`
- Modify: `algorithm/pinyin/src/candidate_pipeline.cpp`
- Modify: `algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/core/learning_store_test.cpp`
- Modify: `tests/core/foundation_test.cpp` only if shared test helpers are needed

**Interfaces:**
- `LearningEntry { std::string phrase; std::string pinyin; std::string contextBefore; std::string contextAfter; std::int64_t frequency; std::int64_t lastSelectedMs; std::int64_t negativeFeedback; }`.
- `LearningSnapshot::boostAt(phrase, pinyin, contextBefore, contextAfter, nowMs) const -> double` returns a bounded score that may be negative after negative feedback.
- `LearningSnapshot::entry(phrase, pinyin, contextBefore, contextAfter) const -> const LearningEntry *` returns the best exact-context or base entry.
- `LearningStore(path).open()`, `recordSelection(...)`, `recordNegativeFeedback(...)`, `snapshot(nowMs)`, and `close()` provide transactional persistence.
- `LearningWriter(path)` owns a worker thread; `enqueueSelection(...)`, `enqueueNegativeFeedback(...)`, `flush()`, and `snapshot()` provide non-blocking writes plus a current immutable snapshot.
- Extend the pinyin pipeline function to accept `const core::LearningSnapshot *`, `std::int64_t nowMs`, `contextBefore`, and `contextAfter`; set `CandidateScore::learning_boost` and `context_bonus` through the snapshot.

- [ ] **Step 1: Add the failing persistence and ranking tests.**

```cpp
void testSelectionPersistsAcrossReopen() {
    const auto path = temporaryPath("learning.sqlite3");
    modernime::core::LearningStore first(path);
    assertTrue(first.open(), "learning store opens");
    assertTrue(first.recordSelection("你好", "ni'hao", "今", "，", 1000),
               "selection is stored");
    first.close();

    modernime::core::LearningStore second(path);
    assertTrue(second.open(), "learning store reopens");
    const auto snapshot = second.snapshot(1000);
    assertTrue(snapshot->boostAt("你好", "nihao", "今", "，", 1000) > 0.0,
               "persisted selection boosts the candidate");
}

void testFrequencyBonusIsBoundedAndRecentSelectionWins() {
    modernime::core::CandidateScore oldCandidate{0, "旧词", "jiu'ci", 0.0F};
    modernime::core::CandidateScore learnedCandidate{5, "常用词", "chang'yong'ci", 0.0F};
    learnedCandidate.learning_boost = 2.0;
    std::vector candidates{oldCandidate, learnedCandidate};
    const auto order = modernime::core::CandidateRanker::rank("changyongci", candidates);
    assertTrue(order.front() == 1, "learned candidate moves ahead");
    assertTrue(learnedCandidate.learning_boost <= 4.0,
               "learning bonus remains bounded");
}
```

- [ ] **Step 2: Run the new tests and verify they fail for the missing implementation.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'learning|candidate' --output-on-failure`

Expected: compilation fails because the learning interfaces and test executable do not exist yet.

- [ ] **Step 3: Implement the SQLite schema and immutable snapshot.**

Use a single table keyed by normalized pinyin, phrase, context-before, and context-after:

```sql
CREATE TABLE IF NOT EXISTS learning_entries (
  pinyin TEXT NOT NULL,
  phrase TEXT NOT NULL,
  context_before TEXT NOT NULL DEFAULT '',
  context_after TEXT NOT NULL DEFAULT '',
  frequency INTEGER NOT NULL DEFAULT 0,
  last_selected_ms INTEGER NOT NULL DEFAULT 0,
  negative_feedback INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (pinyin, phrase, context_before, context_after)
);
```

Implement `recordSelection` as an upsert that increments frequency and replaces `last_selected_ms`; implement negative feedback as an upsert increment. Build a snapshot by reading rows into a vector/map. Normalize pinyin by lowercasing ASCII letters and removing `'` before keying.

- [ ] **Step 4: Implement bounded learning scoring and the asynchronous writer.**

Use a 30-day half-life and bounded contributions:

```cpp
const double recency = std::exp(-ageMs / (30.0 * 24.0 * 60.0 * 60.0 * 1000.0));
const double frequency = std::min(2.0, 0.65 * std::log1p(entry.frequency));
const double recent = std::min(1.0, 0.90 * recency);
const double penalty = std::min(2.0, 0.75 * std::log1p(entry.negativeFeedback));
return std::clamp(frequency + recent - penalty, -2.0, 3.5);
```

The writer updates its in-memory snapshot before queueing disk work, uses one SQLite transaction per drained batch, and makes `flush()` wait until the queue is empty. If open/write fails, keep the in-memory snapshot and return false without throwing.

- [ ] **Step 5: Feed the snapshot into the existing pinyin pipeline.**

Add the optional snapshot/time/context parameters to `buildCandidatePipeline`. For every LibIME candidate, calculate `learning_boost` using text and full pinyin. Keep `CandidateScore::final_score()` as the only final-score location and cap all learning additions there.

- [ ] **Step 6: Connect selection recording in `PinyinCandidateProvider`.**

Create the writer from the resolved learning path. Before calling `context->select`, enqueue the selected candidate’s phrase, full pinyin, and currently stored context. Add `flushLearning()` only to the test-facing provider fixture if needed; production selection remains asynchronous.

- [ ] **Step 7: Run the focused tests, then the full suite.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'learning|candidate|pinyin' --output-on-failure && ctest --preset fcitx5-debug --output-on-failure`

Expected: all tests pass and the existing UI dimension tests remain unchanged.

- [ ] **Step 8: Commit the completed feature.**

```bash
git add core algorithm/pinyin tests CMakeLists.txt
git commit -m "feat: learn user candidate selections"
```

---

### Task 2: Local professional/user dictionary

**Files:**
- Create: `algorithm/pinyin/include/modernime/pinyin/user_dictionary.h`
- Create: `algorithm/pinyin/src/user_dictionary.cpp`
- Modify: `core/include/modernime/core/candidate_model.h`
- Modify: `algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `algorithm/pinyin/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/core/user_dictionary_test.cpp`
- Modify: `tests/core/pinyin_provider_test.cpp`

**Interfaces:**
- `UserDictionaryEntry { std::string pinyin; std::string phrase; float weight; }`.
- Add `enum class CandidateSource { Engine, UserDictionary, Learned };` and append `CandidateSource source = CandidateSource::Engine` to `core::CandidateItem`; existing three-field aggregate initializers remain valid.
- `UserDictionary::loadText(path) -> UserDictionary` skips blank/comment/malformed lines.
- `UserDictionary::addTo(libime::PinyinDictionary &, std::size_t index) const` adds entries with LibIME’s `addWord` API.
- `UserDictionary::contains(normalizedPinyin, phrase) const -> bool` identifies manual candidates.
- `UserDictionary::removeFrom(libime::PinyinDictionary &, std::size_t index, pinyin, phrase) -> bool` removes only the manual dictionary entry.
- Extend `PinyinDataPaths` with `userDictionary` and `learningStore`; default them under `${XDG_DATA_HOME:-$HOME/.local/share}/modernime/`.

- [ ] **Step 1: Add parser and provider integration tests first.**

```cpp
void testUserDictionarySkipsBadRowsAndKeepsLastDuplicate() {
    writeFile(path, "# comment\nni'hao\t你好\t100\ninvalid\nnihao\t您好\t80\nnihao\t你好\t120\n");
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(dictionary.entries().size() == 2, "valid rows are loaded");
    assertTrue(dictionary.contains("nihao", "你好"), "apostrophes normalize");
}

void testProfessionalPhraseAppearsFromConfiguredDictionary() {
    writeFile(path, "rengongzhineng\t人工智能\t100\n");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = path.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    for (const char c : std::string("rengongzhineng")) {
        assertTrue(provider.append(std::string_view(&c, 1)), "pinyin is accepted");
    }
    assertTrue(hasText(provider.page(), "人工智能"), "professional phrase is a candidate");
}
```

- [ ] **Step 2: Run the focused tests and verify the expected failure.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'user_dictionary|pinyin_provider' --output-on-failure`

Expected: compilation fails because `UserDictionary` and the new data paths do not exist.

- [ ] **Step 3: Implement strict UTF-8-safe text parsing.**

Read UTF-8 lines, split exactly on tabs, require exactly three columns, normalize ASCII pinyin, require a non-empty phrase and finite non-negative weight, and use the last valid duplicate. Do not reject non-ASCII phrase text. Expose a read-only `entries()` vector for tests and diagnostics.

- [ ] **Step 4: Add the dictionary as a separate LibIME layer.**

Load the configured file during provider construction and add it at dictionary index 1. Keep the system dictionary at index 0. Do not append custom candidates after LibIME decoding; let LibIME’s Trie, segmentation, and N-best pipeline see them before ranking. A missing file is an empty user dictionary.

- [ ] **Step 5: Mark and merge manual candidates without changing UI geometry.**

When converting LibIME candidates to `CandidateItem`, mark entries found in the user dictionary as `UserDictionary`; deduplicate by normalized full pinyin plus phrase, preserving the highest-priority source. Keep the existing candidate count and rendering metrics unchanged.

- [ ] **Step 6: Verify full and prefix input behavior.**

Add tests for `rengongzhineng`, `rengong`, and a one-letter input. Assert the configured term can appear for a matching prefix; when the normalized input has fewer than three ASCII letters, keep at most two manual-dictionary candidates in the merged result, and when it has three or more letters, keep at most eight. This limit applies only to manual-dictionary candidates and leaves system candidates untouched.

- [ ] **Step 7: Run all tests and commit only this feature.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --preset fcitx5-debug --output-on-failure && git diff --check`

```bash
git add algorithm/pinyin tests
git commit -m "feat: load local user dictionary"
```

---

### Task 3: Candidate paging, current-page selection, and key bindings

**Files:**
- Modify: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Modify: `adapter/fcitx5/include/modernime/fcitx5/fcitx_engine.h`
- Modify: `adapter/fcitx5/src/inputmethod.cpp`
- Modify: `tests/adapter/engine_state_test.cpp`
- Create: `tests/adapter/key_translation_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Add `KeyKind::DeleteCandidate` only in Task 4; Task 3 adds no deletion behavior.
- Add `ModernIMEController::currentPageIndex() const -> std::size_t` and `ModernIMEController::pageSize() const -> std::size_t`, both using the constant 9.
- Add a namespace-level `translateKey(const fcitx::Key &) -> std::optional<KeyEvent>` so key mappings can be tested without a live input context; `ModernIMEInputMethod::translateKey` delegates to it.
- `movePage(direction)` computes `targetPage = clamp(currentPage + direction, 0, lastPage)` and moves to `targetPage * 9`, never to `cursor + 9` blindly.

- [ ] **Step 1: Add failing controller and key-mapping tests.**

```cpp
void testPageDownStartsAtTheNextPage() {
    ManyCandidateProvider provider;
    RecordingHost host;
    ModernIMEController controller(host, &provider);
    type(controller, "n");
    for (int index = 0; index < 8; ++index) {
        assertTrue(controller.handle({KeyKind::NextCandidate, 0, 0}),
                   "cursor reaches the ninth candidate");
    }
    assertTrue(controller.handle({KeyKind::NextPage, 0, 0}), "page down works");
    assertTrue(controller.page().cursor == 9, "page down starts at item ten");
    assertTrue(controller.handle({KeyKind::Digit, 0, '1'}), "page digit works");
    assertTrue(host.commits.back() == "候选10", "digit selects current page item");
}
```

Add translation assertions for `Up`, `Down`, `PageUp`, `PageDown`, `equal`, and `plus`.

- [ ] **Step 2: Run the focused tests and verify they fail.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'engine_state|key_translation' --output-on-failure`

Expected: the current controller selects the wrong global digit after paging and `equal`/`plus` are not translated.

- [ ] **Step 3: Implement page-relative digit selection.**

For digit `n`, calculate `index = (page().cursor / 9) * 9 + (n - 1)`. Return false when that index is outside `page_.items`; otherwise call the existing selection path.

- [ ] **Step 4: Implement page movement and key translation.**

Keep all candidates in the `CommonCandidateList`, keep `setPageSize(9)`, and set the Fcitx page from the resulting global cursor. Map `Up`/`PageUp` to previous page, `Down`/`PageDown`/`equal`/`plus` to next page. Invalid boundary moves return false so Fcitx can handle the key.

- [ ] **Step 5: Verify Fcitx candidate-list and UI behavior.**

Extend the candidate-list test to assert 20 candidates expose three pages and the second page cursor is relative to its page. Assert `pageFromInputPanel` still receives at most nine current-page items; do not change any `CandidateBarMetrics` values.

- [ ] **Step 6: Run all tests and commit.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --preset fcitx5-debug --output-on-failure && git diff --check`

```bash
git add adapter/fcitx5 tests
git commit -m "feat: add candidate page navigation"
```

---

### Task 4: Delete erroneous learned candidates

**Files:**
- Modify: `core/include/modernime/core/candidate_provider.h`
- Modify: `algorithm/pinyin/include/modernime/pinyin/pinyin_candidate_provider.h`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Modify: `adapter/fcitx5/src/inputmethod.cpp`
- Modify: `tests/adapter/engine_state_test.cpp`
- Modify: `tests/core/pinyin_provider_test.cpp`

**Interfaces:**
- Add a non-pure `CandidateProvider::remove(std::size_t index) -> bool` defaulting to false so existing test providers remain source-compatible.
- Add `KeyKind::DeleteCandidate`.
- Add `ModernIMEController::removeCurrent() -> bool`, which passes the global highlighted index to the provider and republishes the page.
- `PinyinCandidateProvider::remove(index)` removes a manual user-dictionary entry or records negative feedback for an automatically learned/system candidate without deleting the system dictionary file.

- [ ] **Step 1: Add failing deletion tests.**

```cpp
void testDeleteCurrentCandidateRecordsNegativeFeedback() {
    FakeProvider providerWithLearnedCandidate;
    RecordingHost host;
    ModernIMEController controller(host, &providerWithLearnedCandidate);
    type(controller, "nihao");
    assertTrue(controller.handle({KeyKind::DeleteCandidate, 0, 0}),
               "delete key is handled");
    assertTrue(providerWithLearnedCandidate.removeCount == 1,
               "provider receives current candidate deletion");
}
```

Add a pinyin-provider test proving a manual dictionary term disappears after deletion while a system candidate remains available with its user boost removed.

- [ ] **Step 2: Run the focused tests and verify failure.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'engine_state|pinyin_provider' --output-on-failure`

Expected: `DeleteCandidate` and provider removal are not implemented.

- [ ] **Step 3: Implement provider removal semantics.**

For a manual term, remove it from dictionary layer 1 and the in-memory user-dictionary index, then rebuild the current LibIME context from the existing raw pinyin. For an automatically learned/system term, enqueue negative feedback and suppress only the learning boost; do not remove the system dictionary entry.

- [ ] **Step 4: Add Ctrl+Delete and Shift+Delete translation.**

Check modifier-specific Delete before plain Backspace. Translate `Ctrl+Delete` and `Shift+Delete` to `DeleteCandidate`; leave plain Delete behavior unchanged unless it was already handled elsewhere.

- [ ] **Step 5: Verify deletion, persistence, and regression behavior.**

After deletion, refresh the page and assert the manual candidate is absent. Reopen the learning database and assert negative feedback persisted. Run the full test suite and `git diff --check`.

- [ ] **Step 6: Commit the feature.**

```bash
git add core algorithm/pinyin adapter/fcitx5 tests
git commit -m "feat: remove erroneous learned candidates"
```

---

### Task 5: Surrounding-text context ranking

**Files:**
- Modify: `core/include/modernime/core/learning_snapshot.h`
- Modify: `core/src/learning_snapshot.cpp`
- Modify: `core/include/modernime/core/candidate_ranker.h`
- Modify: `core/src/candidate_ranker.cpp`
- Modify: `algorithm/pinyin/src/candidate_pipeline.cpp`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `adapter/fcitx5/include/modernime/fcitx5/engine.h`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Modify: `adapter/fcitx5/src/inputmethod.cpp`
- Modify: `tests/core/learning_store_test.cpp`
- Modify: `tests/adapter/engine_state_test.cpp`

**Interfaces:**
- Add `CandidateProvider::setContext(std::string_view before, std::string_view after)` as a default no-op.
- Add `ModernIMEController::setContext(std::string before, std::string after)`; the controller forwards the latest bounded context to the provider before the next append/select refresh.
- Add `extractSurroundingContext(const fcitx::SurroundingText &, std::size_t maxChars) -> std::pair<std::string, std::string>` in the Fcitx adapter implementation.
- `LearningSnapshot::contextBoost(...)` returns only a bounded delta and returns zero for empty/invalid context.

- [ ] **Step 1: Add failing context-ranking tests.**

```cpp
void testMatchingContextRaisesCandidate() {
    const auto path = temporaryPath("context.sqlite3");
    LearningStore store(path);
    assertTrue(store.open(), "context store opens");
    assertTrue(store.recordSelection("你好", "nihao", "今天天气", "很好", 1000),
               "context selection stores");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->contextBoost("你好", "nihao", "今天天气", "很好") > 0.0,
               "matching context adds a bounded boost");
    assertTrue(snapshot->contextBoost("你好", "nihao", "完全不同", "") == 0.0,
               "unmatched context adds no boost");
}
```

Add an adapter test that sets `"前文"`/`"后文"`, types pinyin, and verifies the provider receives the context before candidate generation.

- [ ] **Step 2: Run the focused tests and verify failure.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --test-dir build/fcitx5-debug -R 'learning|engine_state' --output-on-failure`

Expected: no context API exists and the context score remains zero.

- [ ] **Step 3: Implement bounded context extraction.**

If `SurroundingText::isValid()` is false, return two empty strings. Otherwise use the character cursor offset, not byte arithmetic, and retain at most 32 UTF-8 characters before and after the cursor using `fcitx::utf8::nextNChar`. Do not include the active preedit in surrounding text.

- [ ] **Step 4: Forward context through the controller and provider.**

In `keyEvent`, read `event.inputContext()->surroundingText()` before handling the key, set the controller context, and let the next provider refresh pass it to the pipeline. Clear stored context on reset/deactivate.

- [ ] **Step 5: Add the context score with a strict cap.**

Use exact bounded context matches only for the first implementation: add at most `0.8` for exact before/after context, `0.4` for exact before-only or after-only context, and zero otherwise. Keep this contribution below the user-frequency cap and never use it to admit a non-matching pinyin candidate.

- [ ] **Step 6: Run all tests, inspect the diff, and commit.**

Run: `cmake --build --preset fcitx5-debug --parallel 2 && ctest --preset fcitx5-debug --output-on-failure && git diff --check`

```bash
git add core algorithm/pinyin adapter/fcitx5 tests
git commit -m "feat: rank candidates with local context"
```

---

## Final verification and handoff

- [ ] Run `cmake --build --preset fcitx5-debug --parallel 2`.
- [ ] Run `ctest --preset fcitx5-debug --output-on-failure` and record the total passing count.
- [ ] Run `git diff --check`.
- [ ] Install the final branch build with `cmake --install build/fcitx5-debug`.
- [ ] Restart Fcitx5 and verify `fcitx5-remote -c` succeeds.
- [ ] Manually verify: select a non-first candidate, restart Fcitx5, load a professional phrase, page with `Down` and `=`, select a second-page digit, and delete a learned candidate.
- [ ] Confirm no candidate-bar metric value changed and no main-checkout untracked files were staged.
