# Multi-granularity Candidates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mix useful phrase-prefix candidates into long-pinyin results and preserve unconsumed pinyin after partial selection.

**Architecture:** Extend candidate metadata with raw-input consumption, generate prefix candidates from LibIME syllable boundaries, apply a deterministic first-page diversity mixer, and let the controller continue a composition when selection leaves input behind.

**Tech Stack:** C++20, LibIME Pinyin, Fcitx5 adapter, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-09-01-multigranularity-candidates-design.md`

## Global Constraints

- Do not change candidate-window UI.
- Keep all behavior offline.
- Preserve full-candidate selection and existing learning behavior.

---

### Task 1: Candidate content contract

**Files:**
- Modify: `tests/core/pinyin_provider_test.cpp`
- Modify: `tests/adapter/pinyin_engine_integration_test.cpp`

- [ ] Add a real LibIME regression for `ni'hao'a'lao'di` requiring a full top result and useful phrase-prefix candidates on the first page.
- [ ] Add a selection regression requiring `你好啊` to commit while `lao'di` remains active.
- [ ] Run both tests and confirm they fail for the missing behavior.

### Task 2: Candidate metadata and prefix generation

**Files:**
- Modify: `core/include/modernime/core/candidate_model.h`
- Modify: `core/src/candidate_model.cpp`
- Modify: `algorithm/pinyin/include/modernime/pinyin/candidate_pipeline.h`
- Modify: `algorithm/pinyin/src/candidate_pipeline.cpp`
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`

- [ ] Add raw-input and consumed-prefix metadata.
- [ ] Query and score LibIME candidates at valid syllable boundaries.
- [ ] Insert useful prefix candidates while bounding redundant full-length variants.
- [ ] Run the provider regression until green.

### Task 3: Partial selection

**Files:**
- Modify: `algorithm/pinyin/src/pinyin_candidate_provider.cpp`
- Modify: `adapter/fcitx5/src/engine.cpp`
- Modify: `tests/adapter/engine_state_test.cpp`

- [ ] Rebuild the provider from the unconsumed suffix after partial selection.
- [ ] Commit the selected text and keep the refreshed composition active.
- [ ] Preserve the existing clear-on-full-selection behavior.
- [ ] Run provider, engine-state, and integration tests until green.

### Task 4: Verification and deployment

**Files:**
- No source changes expected.

- [ ] Build the Fcitx5 debug preset.
- [ ] Run the complete CTest suite.
- [ ] Run `git diff --check` and inspect the scoped diff.
- [ ] Install the verified build and restart the Fcitx5 runtime.
