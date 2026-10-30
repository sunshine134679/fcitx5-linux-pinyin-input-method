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
    VariantRecallPlacement,
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
    VariantRecallVisible,
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
    std::string_view segmentedInput;

    std::vector<core::CandidateItem> fullCandidates;
    std::vector<core::CandidateItem> partialCandidates;
    std::vector<std::size_t> prefixEnds;
    std::vector<core::CandidateItem> prefixEvidence;

    std::vector<std::string> macroCandidates;
    std::vector<std::string> englishPredictions;
    std::size_t learnedEnglishCount = 0;
    bool isEnglishWord = false;

    bool clipboardEnabled = false;
    std::string clipboardTrigger = "v";

    double topChineseLearningBoost = 0.0;

    // 近音变体召回的最佳候选（2026-10-24 容错专项）：来自 rhymeVariantInput
    // 派生解码（如 kazhu -> 卡着 ka'zhe）。组合器将其置于第 2 位可见席位，
    // 不改变既有语义顺序的所有权；为空表示本轮无变体召回。
    std::optional<core::CandidateItem> variantTopCandidate;

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

// Pure language fact evaluations shared between provider and composer
bool isInitialConsonantInput(std::string_view rawInput);

bool isConsonantAbbreviationInput(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items);

bool hasExactFullPinyinMatch(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items);

bool hasTrustedShortAbbreviation(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items);

bool isTopChineseLexical(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items,
    const std::function<bool(std::string_view text, std::string_view pinyin)> &isLexicalWord = nullptr);

bool hasHighConfidenceChinese(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items,
    const std::function<bool(std::string_view text, std::string_view pinyin)> &isLexicalWord = nullptr);

} // namespace modernime::pinyin
