# Multi-granularity Candidate Content Design

## Goal

For a long pinyin composition, keep the best full-sentence conversion first while making the rest of the first page useful: include high-confidence shorter phrase conversions instead of filling it with near-identical full-sentence homophones.

## Scope

- Candidate generation, ranking/mixing, and selection semantics only.
- No candidate-window visual, size, color, spacing, animation, or positioning changes.
- Offline operation remains the default; no network service is introduced.

## Behavior

For `ni'hao'a'lao'di`, the first page should contain the full result `你好啊老弟` plus useful shorter candidates such as `你好啊` and `你好`. At most one additional full-length candidate sharing the same long textual prefix may occupy the first page.

Partial candidates consume a prefix of the raw pinyin. Selecting `你好啊` commits that text and keeps `lao'di` active for continued conversion. Full candidates retain the existing commit-and-clear behavior.

## Architecture

`CandidateItem` records how many raw input bytes it consumes. The pinyin provider queries LibIME at valid syllable boundaries, ranks each prefix using the existing dictionary, learning, and context signals, and mixes the best phrase-level results into the full candidate stream. A diversity pass limits redundant full-length variants without changing the underlying dictionary.

The controller observes the provider page after selection. If raw input remains, it publishes the refreshed page and keeps composition active; otherwise it follows the existing reset path.

## Acceptance

- Full-sentence top candidate remains stable for the representative long input.
- `你好啊` and `你好` appear on the first page.
- The first page is not dominated by candidates differing only in the final character.
- Partial selection commits only the selected prefix and preserves the remaining pinyin.
- Existing short-input, user-dictionary, learning, editing, and full-commit behavior remains compatible.
