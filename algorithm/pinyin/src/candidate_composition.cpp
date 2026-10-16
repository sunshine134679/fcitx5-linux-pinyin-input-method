#include "modernime/pinyin/candidate_composition.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/candidate_prefixes.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace modernime::pinyin {

std::size_t pinyinSyllableCount(std::string_view fullPinyin) {
    std::string_view trimmed = fullPinyin;
    while (!trimmed.empty() && trimmed.back() == '\'') {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::count(trimmed.begin(), trimmed.end(), '\'') + 1);
}

std::size_t utf8CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char character : text) {
        if ((character & 0xc0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

bool hasInvalidInternalPinyinSegment(std::string_view segmentedInput) {
    if (segmentedInput.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start < segmentedInput.size()) {
        const auto pos = segmentedInput.find('\'', start);
        if (pos == std::string_view::npos) {
            break;
        }
        const auto segment = segmentedInput.substr(start, pos - start);
        start = pos + 1;
        if (segment.empty()) {
            continue;
        }
        const bool hasVowel =
            std::any_of(segment.begin(), segment.end(), [](char c) {
                return c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
                       c == 'u' || c == 'v';
            });
        if (!hasVowel) {
            return true;
        }
    }
    return false;
}

void interleaveHigherNativeRankFuzzyCandidate(
    std::string_view rawInput,
    std::vector<core::CandidateItem> &fullCandidates) {
    if (fullCandidates.size() < 2 ||
        core::PinyinMatchPolicy::priority(
            rawInput, fullCandidates.front().fullPinyin) != 2) {
        return;
    }

    if (pinyinSyllableCount(fullCandidates.front().fullPinyin) <= 1) {
        return;
    }

    const auto fuzzy = std::find_if(
        std::next(fullCandidates.begin()), fullCandidates.end(),
        [&rawInput, &fullCandidates](const auto &candidate) {
            return core::PinyinMatchPolicy::priority(
                       rawInput, candidate.fullPinyin) == 0 &&
                   candidate.sourceIndex < fullCandidates.front().sourceIndex;
        });
    if (fuzzy == fullCandidates.end()) {
        return;
    }

    auto promoted = std::move(*fuzzy);
    fullCandidates.erase(fuzzy);
    fullCandidates.insert(std::next(fullCandidates.begin()),
                          std::move(promoted));
}

CandidateCompositionResult composeCandidates(const CandidateCompositionInput &input) {
    std::vector<CandidateMoveRecord> moves;
    if (input.recordMoves) {
        moves.reserve(64);
    }
    const auto recordMove = [&](core::CandidateSource source, CompositionStage stage,
                                std::size_t index, MoveReason reason) {
        if (input.recordMoves && moves.size() < 256) {
            moves.push_back({source, stage, index, reason});
        }
    };

    // Stage 1: Fuzzy Candidates Interleaving
    auto fullItems = input.fullCandidates;
    const std::string beforeFuzzyRank1 = fullItems.size() >= 2 ? fullItems[1].text : "";
    interleaveHigherNativeRankFuzzyCandidate(input.rawInput, fullItems);
    if (fullItems.size() >= 2 && fullItems[1].text != beforeFuzzyRank1) {
        recordMove(fullItems[1].source, CompositionStage::FuzzyInterleave, 1,
                   MoveReason::FuzzyInterleaveHigherRank);
    }

    // Stage 2: Chinese Multi-Granularity Mixing
    const auto *bestFullSentence = fullItems.empty() ? nullptr : &fullItems.front();
    auto mixed = mixCandidateItems(
        fullItems, input.partialCandidates, bestFullSentence,
        input.prefixEnds, input.rawInput, &input.prefixEvidence);

    for (std::size_t i = 0; i < mixed.size(); ++i) {
        recordMove(mixed[i].source, CompositionStage::ChineseMultiGranularity, i,
                   MoveReason::InitialPlacement);
    }

    // Stage 3: Macro Insertion
    if (!input.macroCandidates.empty()) {
        std::size_t insertPos =
            (mixed.empty() || (!input.rawInput.empty() && input.rawInput.front() == 'v'))
                ? 0
                : 1;
        for (const auto &macroText : input.macroCandidates) {
            if (std::any_of(mixed.begin(), mixed.end(),
                            [&](const auto &existing) { return existing.text == macroText; })) {
                continue;
            }
            core::CandidateItem item;
            item.text = macroText;
            item.fullPinyin = std::string(input.rawInput);
            item.source = core::CandidateSource::Engine;
            item.commitRole = core::CommitCandidateRole::Chinese;
            item.origins = {item.source};
            item.sourceIndex = std::numeric_limits<std::size_t>::max();
            item.consumedInputBytes = input.rawInput.size();
            if (insertPos < mixed.size()) {
                mixed.insert(mixed.begin() + insertPos, std::move(item));
            } else {
                mixed.push_back(std::move(item));
            }
            recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                       insertPos, MoveReason::MacroInsertPos);
            ++insertPos;
        }
    }

    // Stage 4: English & Raw & Command Placement
    const auto rawInput = input.rawInput;
    const bool isInitialConsonant =
        (rawInput == "sh" || rawInput == "ch" || rawInput == "zh");
    const bool isConsonantAbbreviation =
        rawInput.size() >= 2 && !mixed.empty() &&
        std::all_of(rawInput.begin(), rawInput.end(), [](char c) {
            return c >= 'a' && c <= 'z' &&
                   c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' &&
                   c != 'v' && c != 'y';
        }) &&
        (rawInput.size() == 2 ||
         core::PinyinMatchPolicy::abbreviationKey(mixed.front().fullPinyin) == rawInput ||
         core::PinyinMatchPolicy::initialsMatch(rawInput, mixed.front().fullPinyin));
    const bool isEnglish =
        !isInitialConsonant && !isConsonantAbbreviation && input.isEnglishWord;

    const bool hasExactFullPinyinMatch = std::any_of(
        mixed.begin(), mixed.end(),
        [&rawInput](const auto &item) {
            return item.source != core::CandidateSource::Raw &&
                   !hasInvalidInternalPinyinSegment(item.fullPinyin) &&
                   (core::PinyinMatchPolicy::exactInputMatch(rawInput, item.fullPinyin) ||
                    core::PinyinMatchPolicy::isFullTypoMatch(rawInput, item.fullPinyin));
        });

    const bool hasTrustedShortAbbreviation = std::any_of(
        mixed.begin(), mixed.end(),
        [&rawInput](const auto &item) {
            return item.source != core::CandidateSource::Raw &&
                   core::PinyinMatchPolicy::trustedShortAbbreviationMatch(
                       rawInput, item.fullPinyin, item.text);
        });

    bool isTopChineseLexical = false;
    if (!mixed.empty()) {
        const auto &topChinese = mixed.front();
        if (topChinese.source == core::CandidateSource::UserDictionary ||
            topChinese.isLexical ||
            (utf8CodePointCount(topChinese.text) == 1 && rawInput.size() <= 3)) {
            isTopChineseLexical = true;
        } else if (input.isLexicalWord) {
            isTopChineseLexical = input.isLexicalWord(topChinese.text, topChinese.fullPinyin);
        }
    }

    const bool hasHighConfidenceChinese =
        !mixed.empty() &&
        (hasTrustedShortAbbreviation || isConsonantAbbreviation ||
         (hasExactFullPinyinMatch && isTopChineseLexical));

    const bool isEnglishSuppressed =
        input.isSuppressed ? input.isSuppressed(rawInput, rawInput) : false;

    bool preferEnglishOverChinese = false;
    if (mixed.empty()) {
        preferEnglishOverChinese = true;
    } else if (hasHighConfidenceChinese) {
        preferEnglishOverChinese = false;
    } else if (!input.englishPredictions.empty() && !hasExactFullPinyinMatch) {
        preferEnglishOverChinese = true;
    } else if (isEnglish && !isEnglishSuppressed) {
        if (!hasExactFullPinyinMatch) {
            preferEnglishOverChinese = true;
        } else if (!isTopChineseLexical) {
            const double chineseLearningBoost =
                (mixed.front().learningBoost > 0.0)
                    ? mixed.front().learningBoost
                    : input.topChineseLearningBoost;
            if (chineseLearningBoost < 2.5) {
                preferEnglishOverChinese = true;
            }
        }
    }

    core::CandidateItem rawCandidate;
    rawCandidate.text = rawInput;
    rawCandidate.fullPinyin = std::string(rawInput);
    rawCandidate.source = core::CandidateSource::Raw;
    rawCandidate.commitRole = core::CommitCandidateRole::Raw;
    rawCandidate.origins = {rawCandidate.source};
    rawCandidate.consumedInputBytes = rawInput.size();

    mixed.erase(
        std::remove_if(mixed.begin(), mixed.end(),
                       [](const auto &item) {
                           return item.source == core::CandidateSource::Raw;
                       }),
        mixed.end());

    char triggerChar = 'v';
    if (input.clipboardTrigger.size() == 3 && input.clipboardTrigger[1] == '+') {
        triggerChar = static_cast<char>(
            std::tolower(static_cast<unsigned char>(input.clipboardTrigger[0])));
    }
    const bool isSingleTriggerChar =
        rawInput.size() == 1 &&
        std::tolower(static_cast<unsigned char>(rawInput.front())) == triggerChar;

    std::string preedit;
    if (isSingleTriggerChar && rawInput == "v") {
        mixed.clear();
        core::CandidateItem rawItem;
        rawItem.text = "v";
        rawItem.fullPinyin = "v";
        rawItem.source = core::CandidateSource::Raw;
        rawItem.commitRole = core::CommitCandidateRole::Raw;
        rawItem.origins = {rawItem.source};
        rawItem.consumedInputBytes = 1;
        mixed.push_back(std::move(rawItem));
        recordMove(core::CandidateSource::Raw, CompositionStage::CommandPlacement, 0,
                   MoveReason::CommandSingleTrigger);

        if (input.clipboardEnabled) {
            core::CandidateItem cmdCandidate;
            cmdCandidate.text = "剪贴板 (" + input.clipboardTrigger + ")";
            cmdCandidate.fullPinyin = "v";
            cmdCandidate.source = core::CandidateSource::Command;
            cmdCandidate.commitRole = core::CommitCandidateRole::Command;
            cmdCandidate.origins = {cmdCandidate.source};
            cmdCandidate.consumedInputBytes = 1;
            mixed.push_back(std::move(cmdCandidate));
            recordMove(core::CandidateSource::Command, CompositionStage::CommandPlacement, 1,
                       MoveReason::CommandSingleTrigger);
        }
        preedit = "v";
    } else if (!input.macroCandidates.empty() && !rawInput.empty() && rawInput.front() == 'v') {
        mixed.push_back(std::move(rawCandidate));
        recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                   mixed.size() - 1, MoveReason::InitialPlacement);
        preedit = std::string(rawInput);
    } else if (preferEnglishOverChinese) {
        if (!input.englishPredictions.empty()) {
            core::CandidateItem predictedCandidate;
            predictedCandidate.text = input.englishPredictions.front();
            predictedCandidate.fullPinyin = predictedCandidate.text;
            predictedCandidate.source = core::CandidateSource::Raw;
            predictedCandidate.commitRole = core::CommitCandidateRole::English;
            predictedCandidate.origins = {predictedCandidate.source};
            predictedCandidate.consumedInputBytes = rawInput.size();

            mixed.insert(mixed.begin(), std::move(rawCandidate));
            mixed.insert(mixed.begin(), std::move(predictedCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);
        } else {
            mixed.insert(mixed.begin(), std::move(rawCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);
        }
        preedit = std::string(rawInput);
    } else {
        if (!input.englishPredictions.empty()) {
            core::CandidateItem predictedCandidate;
            predictedCandidate.text = input.englishPredictions.front();
            predictedCandidate.fullPinyin = predictedCandidate.text;
            predictedCandidate.source = core::CandidateSource::Raw;
            predictedCandidate.commitRole = core::CommitCandidateRole::English;
            predictedCandidate.origins = {predictedCandidate.source};
            predictedCandidate.consumedInputBytes = rawInput.size();

            const auto insertPos =
                mixed.empty() ? mixed.begin() : std::next(mixed.begin());
            const auto idx = static_cast<std::size_t>(std::distance(mixed.begin(), insertPos));
            mixed.insert(insertPos, std::move(predictedCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       idx, MoveReason::EnglishNormalPlacement);
        }

        if (isEnglish || !input.englishPredictions.empty()) {
            bool isSingleSyllable = false;
            if (!mixed.empty()) {
                isSingleSyllable = pinyinSyllableCount(mixed.front().fullPinyin) <= 1;
            }
            const bool isDualAttributeExemption = (rawInput == "can");
            if (isSingleSyllable && !isDualAttributeExemption) {
                mixed.push_back(std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           mixed.size() - 1, MoveReason::EnglishNormalPlacement);
            } else {
                const auto insertPos =
                    mixed.empty() ? mixed.begin() : std::next(mixed.begin());
                const auto idx = static_cast<std::size_t>(std::distance(mixed.begin(), insertPos));
                mixed.insert(insertPos, std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           idx, MoveReason::EnglishNormalPlacement);
            }
        } else {
            mixed.push_back(std::move(rawCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       mixed.size() - 1, MoveReason::InitialPlacement);
        }
        preedit = std::string(input.segmentedInput);
    }

    // Stage 5: English definition annotation lookup
    if (input.englishDefinition.has_value() && !mixed.empty()) {
        auto &top = mixed.front();
        if (top.source == core::CandidateSource::Raw &&
            top.text.size() >= 4 && input.isEnglishWord) {
            top.annotation = *input.englishDefinition;
            recordMove(top.source, CompositionStage::EnglishDefinitionAnnotation, 0,
                       MoveReason::InitialPlacement);
        }
    }

    // Stage 6: Final suppression filtering (unified boundary)
    if (input.isSuppressed) {
        mixed.erase(
            std::remove_if(mixed.begin(), mixed.end(),
                           [&](const auto &item) {
                               if (item.source != core::CandidateSource::Command &&
                                   input.isSuppressed(item.text, item.fullPinyin)) {
                                   recordMove(item.source, CompositionStage::FinalSuppression,
                                              0, MoveReason::SuppressedCandidateDropped);
                                   return true;
                               }
                               return false;
                           }),
            mixed.end());
    }

    // Stage 7: Deduplication and origin merging via CandidateIdentity
    std::unordered_set<core::CandidateIdentity> seenCandidateIdentities;
    std::vector<core::CandidateItem> deduplicated;
    deduplicated.reserve(mixed.size());
    for (auto &item : mixed) {
        if (item.source == core::CandidateSource::Command) {
            deduplicated.push_back(std::move(item));
            continue;
        }
        const auto id = core::candidateIdentity(item, rawInput.size());
        if (seenCandidateIdentities.insert(id).second) {
            deduplicated.push_back(std::move(item));
        } else {
            recordMove(item.source, CompositionStage::DeduplicationAndOriginMerge,
                       deduplicated.size(), MoveReason::DuplicateMerged);
            for (auto &existing : deduplicated) {
                if (core::candidateIdentity(existing, rawInput.size()) == id) {
                    for (auto src : item.origins) {
                        if (std::find(existing.origins.begin(), existing.origins.end(), src) ==
                            existing.origins.end()) {
                            existing.origins.push_back(src);
                        }
                    }
                    if (std::find(existing.origins.begin(), existing.origins.end(), item.source) ==
                        existing.origins.end()) {
                        existing.origins.push_back(item.source);
                    }
                    break;
                }
            }
        }
    }
    mixed = std::move(deduplicated);

    // Stage 8: Ambiguity annotations for items with identical display text
    std::unordered_map<std::string, std::size_t> textCounts;
    for (const auto &item : mixed) {
        if (item.source != core::CandidateSource::Command) {
            ++textCounts[item.text];
        }
    }
    for (std::size_t i = 0; i < mixed.size(); ++i) {
        auto &item = mixed[i];
        if (item.source == core::CandidateSource::Command) {
            continue;
        }
        if (textCounts[item.text] > 1 && item.annotation.empty()) {
            if (item.consumedInputBytes > 0 && item.consumedInputBytes < rawInput.size()) {
                item.annotation = std::string(rawInput.substr(item.consumedInputBytes));
                recordMove(item.source, CompositionStage::AmbiguityAnnotation, i,
                           MoveReason::AmbiguityRemainderAnnotated);
            } else if (!item.fullPinyin.empty()) {
                item.annotation = item.fullPinyin;
                recordMove(item.source, CompositionStage::AmbiguityAnnotation, i,
                           MoveReason::AmbiguityReadingAnnotated);
            }
        }
    }

    return CandidateCompositionResult{
        .items = std::move(mixed),
        .preedit = std::move(preedit),
        .moves = std::move(moves),
    };
}

} // namespace modernime::pinyin
