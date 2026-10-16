#pragma once

#include "modernime/core/candidate_model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::pinyin {

enum class CompositionStage {
    FuzzyInterleave,
    ChineseMultiGranularity,
    MacroInsert,
    EnglishAndRawPlacement,
    CommandPlacement,
    EnglishDefinitionAnnotation,
    FinalSuppression,
    DeduplicationAndOriginMerge,
    AmbiguityAnnotation,
};

enum class MoveReason {
    InitialPlacement,
    FuzzyInterleaveHigherRank,
    MacroInsertPos,
    EnglishPreferOverChinese,
    EnglishNormalPlacement,
    CommandSingleTrigger,
    SuppressedCandidateDropped,
    DuplicateMerged,
    AmbiguityReadingAnnotated,
    AmbiguityRemainderAnnotated,
};

struct CandidateMoveRecord {
    core::CandidateSource source;
    CompositionStage stage;
    std::size_t index;
    MoveReason reason;

    bool operator==(const CandidateMoveRecord &) const = default;
};

struct CandidateCompositionInput {
    std::string_view rawInput;
    std::string_view canonicalInput;
    std::string_view segmentedInput;

    std::vector<core::CandidateItem> fullCandidates;
    std::vector<core::CandidateItem> partialCandidates;
    std::vector<std::size_t> prefixEnds;
    std::vector<core::CandidateItem> prefixEvidence;

    std::vector<std::string> macroCandidates;
    std::vector<std::string> englishPredictions;
    bool isEnglishWord = false;
    bool hasPinyinCoverage = true;
    bool hasInvalidInternalPinyinSegment = false;

    bool clipboardEnabled = false;
    std::string clipboardTrigger = "v";

    double topChineseLearningBoost = 0.0;

    std::optional<std::string> englishDefinition;
    std::function<std::string(std::string_view word)> resolveEnglishDefinition;

    std::function<bool(std::string_view text, std::string_view pinyin)> isSuppressed;
    std::function<bool(std::string_view text, std::string_view pinyin)> isLexicalWord;

    bool recordMoves = false;
};

struct CandidateCompositionResult {
    std::vector<core::CandidateItem> items;
    std::string preedit;
    std::vector<CandidateMoveRecord> moves;
};

// Pure candidate composition boundary:
// Fuzzy -> Chinese Multi-Granularity -> Macro -> English/Raw/Command -> Final Suppression / Dedup
CandidateCompositionResult composeCandidates(const CandidateCompositionInput &input);

// Exported helper for fuzzy candidate interleaving
void interleaveHigherNativeRankFuzzyCandidate(
    std::string_view rawInput,
    std::vector<core::CandidateItem> &fullCandidates);

// Helper for invalid internal pinyin segment check
bool hasInvalidInternalPinyinSegment(std::string_view segmentedInput);

// Helper for pinyin syllable count
std::size_t pinyinSyllableCount(std::string_view fullPinyin);

// Helper for utf8 code point count
std::size_t utf8CodePointCount(std::string_view text);

} // namespace modernime::pinyin
