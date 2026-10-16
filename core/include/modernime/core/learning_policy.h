#pragma once

#include "modernime/core/commit_event.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace modernime::core {

enum class CommitLearningDecision {
    IgnoreCommand,
    IgnoreRawPinyin,
    IgnoreCoreDefault,
    Record
};

// Pure policy decision for learning from a CommitEvent.
// hasPositiveFrequency indicates whether the (text, pinyinKey) already has
// positive frequency in the current snapshot/store.
CommitLearningDecision decideCommitLearning(const CommitEvent &event,
                                            bool hasPositiveFrequency);

// Core default words protected from unconditioned frequency inflation
// when selected at rank 0 without context, deviation, or pre-existing learning.
bool isProtectedCoreDefault(std::string_view text, std::string_view pinyin);

// Canonical lowercase ASCII word for an explicit full-word selection.
std::string canonicalEnglishSelectionWord(std::string_view phrase,
                                          std::string_view pinyin);

// Defined for file intent in T05/T06
enum class PersonalFileIntent {
    RestoreTrusted,
    ImportExternal
};

} // namespace modernime::core
