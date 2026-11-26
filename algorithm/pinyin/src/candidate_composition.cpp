#include "modernime/pinyin/candidate_composition.h"

#include "modernime/core/english_dictionary.h"
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

bool isInitialConsonantInput(std::string_view rawInput) {
    return rawInput == "sh" || rawInput == "ch" || rawInput == "zh";
}

bool isConsonantAbbreviationInput(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items) {
    if (rawInput.size() < 2 || items.empty()) {
        return false;
    }
    const bool allConsonants = std::all_of(rawInput.begin(), rawInput.end(), [len = rawInput.size()](char c) {
        return c >= 'a' && c <= 'z' &&
               c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' &&
               c != 'v' && (c != 'y' || len == 3);
    });
    if (!allConsonants) {
        return false;
    }
    return rawInput.size() == 2 ||
           core::PinyinMatchPolicy::abbreviationKey(items.front().fullPinyin) == rawInput ||
           core::PinyinMatchPolicy::initialsMatch(rawInput, items.front().fullPinyin);
}

bool hasExactFullPinyinMatch(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items) {
    return std::any_of(
        items.begin(), items.end(),
        [&rawInput](const auto &item) {
            return item.source != core::CandidateSource::Raw &&
                   !hasInvalidInternalPinyinSegment(item.fullPinyin) &&
                   (core::PinyinMatchPolicy::exactInputMatch(rawInput, item.fullPinyin) ||
                    core::PinyinMatchPolicy::isFullTypoMatch(rawInput, item.fullPinyin));
        });
}

bool hasTrustedShortAbbreviation(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items) {
    return std::any_of(
        items.begin(), items.end(),
        [&rawInput](const auto &item) {
            return item.source != core::CandidateSource::Raw &&
                   core::PinyinMatchPolicy::trustedShortAbbreviationMatch(
                       rawInput, item.fullPinyin, item.text);
        });
}

bool isTopChineseLexical(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items,
    const std::function<bool(std::string_view text, std::string_view pinyin)> &isLexicalWord) {
    if (items.empty()) {
        return false;
    }
    const auto &topChinese = items.front();
    if (topChinese.source == core::CandidateSource::UserDictionary ||
        topChinese.isLexical ||
        (utf8CodePointCount(topChinese.text) == 1 && rawInput.size() <= 3)) {
        return true;
    }
    if (isLexicalWord) {
        return isLexicalWord(topChinese.text, topChinese.fullPinyin);
    }
    return false;
}

bool isLexicalAbbreviationInput(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items,
    const std::function<bool(std::string_view text, std::string_view pinyin)> &isLexicalWord) {
    // Pure consonants retain the existing abbreviation policy, including its
    // y/length boundary for English prefixes. Extend only vowel/mixed spelling
    // or an explicitly separated syllable sequence.
    if (items.empty() || rawInput.find_first_of("aeiouv'") == std::string_view::npos ||
        core::EnglishDictionary::isEnglishWord(rawInput)) {
        return false;
    }
    const auto &top = items.front();
    return top.source != core::CandidateSource::Raw &&
           (top.consumedInputBytes == 0 || top.consumedInputBytes == rawInput.size()) &&
           pinyinSyllableCount(top.fullPinyin) >= 2 &&
           !core::PinyinMatchPolicy::exactInputMatch(rawInput, top.fullPinyin) &&
           core::PinyinMatchPolicy::syllableInputMatch(rawInput, top.fullPinyin) &&
           isTopChineseLexical(rawInput, items, isLexicalWord);
}

bool hasHighConfidenceChinese(
    std::string_view rawInput,
    const std::vector<core::CandidateItem> &items,
    const std::function<bool(std::string_view text, std::string_view pinyin)> &isLexicalWord) {
    if (items.empty()) {
        return false;
    }
    return hasTrustedShortAbbreviation(rawInput, items) ||
           isConsonantAbbreviationInput(rawInput, items) ||
           isLexicalAbbreviationInput(rawInput, items, isLexicalWord) ||
           (hasExactFullPinyinMatch(rawInput, items) && isTopChineseLexical(rawInput, items, isLexicalWord));
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
    if (!input.shuangpin) interleaveHigherNativeRankFuzzyCandidate(input.rawInput, fullItems);
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

    // Stage 2.5: Variant Recall Placement（2026-10-24 容错专项）
    // 近音变体候选（kazhu 输入下经 ka'zhe 解码召回的 卡着）默认沉在
    // prio=0 组尾部不可见；组合器将其放在精确词典同音词之后的可见席位，
    // 不抢占精确同音词的第 2 位。后续 Deduplication 阶段按身份合并重复项。
    if (input.variantTopCandidate.has_value() &&
        !input.variantTopCandidate->text.empty()) {
        core::CandidateItem variantItem = *input.variantTopCandidate;
        std::size_t insertIndex = mixed.empty() ? 0 : 1;
        const std::size_t maxSlot = std::min<std::size_t>(mixed.size(), 5);
        while (insertIndex < maxSlot &&
               mixed[insertIndex].consumedInputBytes == input.rawInput.size() &&
               mixed[insertIndex].isLexical &&
               core::PinyinMatchPolicy::priority(input.rawInput, mixed[insertIndex].fullPinyin) == 2) {
            ++insertIndex;
        }
        const auto insertPos = mixed.begin() + static_cast<std::ptrdiff_t>(insertIndex);
        mixed.insert(insertPos, std::move(variantItem));
        recordMove(input.variantTopCandidate->source,
                   CompositionStage::VariantRecallPlacement, insertIndex,
                   MoveReason::VariantRecallVisible);
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
            item.commitRole = core::CommitCandidateRole::Generated;
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
    const bool hasUppercase = std::any_of(rawInput.begin(), rawInput.end(),
        [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
    const auto containsAsciiAlpha = [](std::string_view text) {
        return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        });
    };
    const auto containsNonAscii = [](std::string_view text) {
        return std::any_of(text.begin(), text.end(),
                           [](unsigned char ch) { return ch >= 0x80; });
    };
    const auto equalsIgnoreCaseAscii = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };
    const bool isInitialConsonant = isInitialConsonantInput(rawInput);
    const bool isChineseAbbreviation = isConsonantAbbreviationInput(rawInput, mixed) ||
        isLexicalAbbreviationInput(rawInput, mixed, input.isLexicalWord);
    const bool isEnglish = !isInitialConsonant && !isChineseAbbreviation && input.isEnglishWord;
    const bool hasExactFullPinyinMatch = (input.shuangpin && !fullItems.empty()) ||
        pinyin::hasExactFullPinyinMatch(rawInput, mixed);
    const bool isTopChineseLexical = pinyin::isTopChineseLexical(rawInput, mixed, input.isLexicalWord);
    const bool hasHighConfidenceChinese = (input.shuangpin && !fullItems.empty()) ||
        pinyin::hasHighConfidenceChinese(rawInput, mixed, input.isLexicalWord);

    const bool isEnglishSuppressed =
        input.isSuppressed ? input.isSuppressed(rawInput, rawInput) : false;

    bool preferEnglishOverChinese = false;
    const bool matureLearnedTop =
        !mixed.empty() &&
        containsAsciiAlpha(mixed.front().text) &&
        mixed.front().isLexical &&
        mixed.front().consumedInputBytes == rawInput.size() &&
        (mixed.front().source == core::CandidateSource::Learned ||
         mixed.front().source == core::CandidateSource::UserDictionary) &&
        mixed.front().learningBoost >= 1.5;
    if (hasUppercase && !matureLearnedTop) {
        preferEnglishOverChinese = true;
    } else if (mixed.empty()) {
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
    if (rawInput == "v" && !input.shuangpin &&
        (isSingleTriggerChar || input.calculatorEnabled || input.symbolSearchEnabled)) {
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

        if (input.clipboardEnabled && isSingleTriggerChar) {
            core::CandidateItem cmdCandidate;
            cmdCandidate.text = "剪贴板 (" + input.clipboardTrigger + ")";
            cmdCandidate.fullPinyin = "v";
            cmdCandidate.source = core::CandidateSource::Command;
            cmdCandidate.commitRole = core::CommitCandidateRole::Command;
            cmdCandidate.origins = {cmdCandidate.source};
            cmdCandidate.consumedInputBytes = 1;
            cmdCandidate.action = core::SelectionAction::OpenClipboard;
            mixed.push_back(std::move(cmdCandidate));
            recordMove(core::CandidateSource::Command, CompositionStage::CommandPlacement, 1,
                       MoveReason::CommandSingleTrigger);
        }
        if (input.calculatorEnabled) {
            core::CandidateItem calculator;
            calculator.text = "离线计算器 (V=)";
            calculator.fullPinyin = "v";
            calculator.source = core::CandidateSource::Command;
            calculator.commitRole = core::CommitCandidateRole::Command;
            calculator.origins = {calculator.source};
            calculator.consumedInputBytes = 1;
            calculator.action = core::SelectionAction::OpenCalculator;
            mixed.push_back(std::move(calculator));
            recordMove(core::CandidateSource::Command, CompositionStage::CommandPlacement,
                       mixed.size() - 1, MoveReason::CommandSingleTrigger);
        }
        if (input.symbolSearchEnabled) {
            for (const auto action : {core::SelectionAction::OpenSymbols, core::SelectionAction::OpenFavoriteSymbols}) {
                core::CandidateItem command;
                command.text = action == core::SelectionAction::OpenSymbols ? "符号搜索 (V;)" : "收藏符号";
                command.fullPinyin = "v";
                command.source = core::CandidateSource::Command;
                command.commitRole = core::CommitCandidateRole::Command;
                command.origins = {command.source}; command.consumedInputBytes = 1; command.action = action;
                mixed.push_back(std::move(command));
                recordMove(core::CandidateSource::Command, CompositionStage::CommandPlacement,
                           mixed.size() - 1, MoveReason::CommandSingleTrigger);
            }
        }
        preedit = "v";
    } else if (!input.macroCandidates.empty() && !rawInput.empty() && rawInput.front() == 'v') {
        mixed.push_back(std::move(rawCandidate));
        recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                   mixed.size() - 1, MoveReason::InitialPlacement);
        preedit = std::string(rawInput);
    } else if (preferEnglishOverChinese) {
        std::vector<std::string> preds;
        std::size_t learnedPredCount = 0;
        for (std::size_t idx = 0; idx < input.englishPredictions.size() && preds.size() < 3; ++idx) {
            const auto &p = input.englishPredictions[idx];
            if (std::find(preds.begin(), preds.end(), p) == preds.end()) {
                if (idx < input.learnedEnglishCount) {
                    ++learnedPredCount;
                }
                preds.push_back(p);
            }
        }

        std::vector<core::CandidateItem> macros;
        if (!input.macroCandidates.empty()) {
            std::vector<core::CandidateItem> nonMacros;
            for (auto &item : mixed) {
                if (std::find(input.macroCandidates.begin(), input.macroCandidates.end(), item.text) !=
                    input.macroCandidates.end()) {
                    macros.push_back(std::move(item));
                } else {
                    nonMacros.push_back(std::move(item));
                }
            }
            mixed = std::move(nonMacros);
        }

        const bool hasLearnedEnglish = input.learnedEnglishCount > 0 && !preds.empty();

        if (hasUppercase) {
            std::vector<core::CandidateItem> englishItems;
            std::vector<std::string> exactCasePreds;
            std::vector<std::string> otherCasePreds;
            bool hasLearnedExactCase = false;
            for (std::size_t idx = 0; idx < preds.size(); ++idx) {
                const auto &p = preds[idx];
                if (p == rawInput) continue;
                if (p.starts_with(rawInput)) {
                    if (idx < learnedPredCount) {
                        hasLearnedExactCase = true;
                    }
                    exactCasePreds.push_back(p);
                } else {
                    otherCasePreds.push_back(p);
                }
            }
            const bool promoteExactLearned =
                hasLearnedExactCase && !exactCasePreds.empty() && rawInput.size() >= 2;
            const auto pushEnglishItem = [&](const std::string &word) {
                core::CandidateItem pItem;
                pItem.text = word;
                pItem.fullPinyin = word;
                pItem.source = core::CandidateSource::Raw;
                pItem.commitRole = equalsIgnoreCaseAscii(word, rawInput)
                                       ? core::CommitCandidateRole::Raw
                                       : core::CommitCandidateRole::English;
                pItem.origins = {pItem.source};
                pItem.consumedInputBytes = rawInput.size();
                englishItems.push_back(std::move(pItem));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           englishItems.size() - 1, MoveReason::EnglishPreferOverChinese);
            };
            if (promoteExactLearned) {
                std::size_t promotedIdx = 0;
                for (std::size_t idx = 0; idx < preds.size(); ++idx) {
                    if (idx < learnedPredCount && preds[idx] != rawInput &&
                        preds[idx].starts_with(rawInput)) {
                        promotedIdx = static_cast<std::size_t>(
                            std::find(exactCasePreds.begin(), exactCasePreds.end(), preds[idx]) -
                            exactCasePreds.begin());
                        break;
                    }
                }
                pushEnglishItem(exactCasePreds[promotedIdx]);
                englishItems.push_back(std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           englishItems.size() - 1, MoveReason::EnglishPreferOverChinese);
                for (std::size_t i = 0; i < exactCasePreds.size(); ++i) {
                    if (i != promotedIdx) {
                        pushEnglishItem(exactCasePreds[i]);
                    }
                }
            } else {
                englishItems.push_back(std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           0, MoveReason::EnglishPreferOverChinese);
                if (rawInput.size() == 1 && rawInput[0] >= 'A' && rawInput[0] <= 'Z') {
                    core::CandidateItem lowerSingle;
                    lowerSingle.text = std::string(1, static_cast<char>(rawInput[0] + ('a' - 'A')));
                    lowerSingle.fullPinyin = lowerSingle.text;
                    lowerSingle.source = core::CandidateSource::Raw;
                    lowerSingle.commitRole = core::CommitCandidateRole::Raw;
                    lowerSingle.origins = {lowerSingle.source};
                    lowerSingle.consumedInputBytes = rawInput.size();
                    englishItems.push_back(std::move(lowerSingle));
                    recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                               englishItems.size() - 1, MoveReason::EnglishPreferOverChinese);
                }
                for (const auto &p : exactCasePreds) {
                    pushEnglishItem(p);
                }
            }
            for (const auto &p : otherCasePreds) {
                pushEnglishItem(p);
            }
            for (auto &m : macros) {
                englishItems.push_back(std::move(m));
                recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                           englishItems.size() - 1, MoveReason::MacroInsertPos);
            }
            // Intentional Shift+letter input targets English/mixed terms, so strip
            // pure-Chinese noise (e.g. "了/你/来" under single "L"), partial-CJK+ASCII
            // fallback glue (e.g. "给itHub" under "GitHub"), and duplicate entries
            // already covered by englishItems, while preserving genuine lexical
            // mixed words (e.g. "Docker容器" under "DockerRongQi").
            mixed.erase(
                std::remove_if(mixed.begin(), mixed.end(),
                               [&](const auto &item) {
                                   if (item.source == core::CandidateSource::Command) return false;
                                   if (containsNonAscii(item.text)) {
                                       const bool genuineMixed =
                                           item.isLexical &&
                                           containsAsciiAlpha(item.text) &&
                                           item.consumedInputBytes == rawInput.size();
                                       if (!genuineMixed) return true;
                                   }
                                   return std::any_of(
                                       englishItems.begin(), englishItems.end(),
                                       [&](const auto &eng) { return eng.text == item.text; });
                               }),
                mixed.end());
            mixed.insert(mixed.begin(),
                         std::make_move_iterator(englishItems.begin()),
                         std::make_move_iterator(englishItems.end()));
        } else if (hasLearnedEnglish) {
            std::vector<core::CandidateItem> englishItems;
            core::CandidateItem topPred;
            topPred.text = preds[0];
            topPred.fullPinyin = preds[0];
            topPred.source = core::CandidateSource::Raw;
            topPred.commitRole = core::CommitCandidateRole::English;
            topPred.origins = {topPred.source};
            topPred.consumedInputBytes = rawInput.size();
            englishItems.push_back(std::move(topPred));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);

            if (rawInput != preds[0]) {
                englishItems.push_back(std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           1, MoveReason::EnglishPreferOverChinese);
            }

            for (std::size_t i = 1; i < preds.size(); ++i) {
                if (preds[i] != rawInput) {
                    core::CandidateItem pItem;
                    pItem.text = preds[i];
                    pItem.fullPinyin = preds[i];
                    pItem.source = core::CandidateSource::Raw;
                    pItem.commitRole = core::CommitCandidateRole::English;
                    pItem.origins = {pItem.source};
                    pItem.consumedInputBytes = rawInput.size();
                    englishItems.push_back(std::move(pItem));
                    recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                               englishItems.size() - 1, MoveReason::EnglishPreferOverChinese);
                }
            }

            for (auto &m : macros) {
                englishItems.push_back(std::move(m));
                recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                           englishItems.size() - 1, MoveReason::MacroInsertPos);
            }

            mixed.insert(mixed.begin(),
                         std::make_move_iterator(englishItems.begin()),
                         std::make_move_iterator(englishItems.end()));
        } else if (isEnglish) {
            std::vector<core::CandidateItem> englishItems;
            englishItems.push_back(std::move(rawCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);

            for (auto &m : macros) {
                englishItems.push_back(std::move(m));
                recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                           englishItems.size() - 1, MoveReason::MacroInsertPos);
            }

            const std::size_t macroCount = macros.size();
            mixed.insert(mixed.begin(),
                         std::make_move_iterator(englishItems.begin()),
                         std::make_move_iterator(englishItems.end()));

            auto predInsertPos = mixed.begin() + std::min(mixed.size(), 1 + macroCount + (mixed.size() > 1 + macroCount ? 1 : 0));
            for (const auto &p : preds) {
                if (p != rawInput) {
                    core::CandidateItem pItem;
                    pItem.text = p;
                    pItem.fullPinyin = p;
                    pItem.source = core::CandidateSource::Raw;
                    pItem.commitRole = core::CommitCandidateRole::English;
                    pItem.origins = {pItem.source};
                    pItem.consumedInputBytes = rawInput.size();
                    const auto idx = static_cast<std::size_t>(std::distance(mixed.begin(), predInsertPos));
                    predInsertPos = std::next(mixed.insert(predInsertPos, std::move(pItem)));
                    recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                               idx, MoveReason::EnglishPreferOverChinese);
                }
            }
        } else if (!preds.empty()) {
            std::vector<core::CandidateItem> englishItems;
            core::CandidateItem topPred;
            topPred.text = preds[0];
            topPred.fullPinyin = preds[0];
            topPred.source = core::CandidateSource::Raw;
            topPred.commitRole = core::CommitCandidateRole::English;
            topPred.origins = {topPred.source};
            topPred.consumedInputBytes = rawInput.size();
            englishItems.push_back(std::move(topPred));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);

            if (rawInput != preds[0]) {
                englishItems.push_back(std::move(rawCandidate));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           1, MoveReason::EnglishPreferOverChinese);
            }

            for (std::size_t i = 1; i < preds.size(); ++i) {
                core::CandidateItem pItem;
                pItem.text = preds[i];
                pItem.fullPinyin = preds[i];
                pItem.source = core::CandidateSource::Raw;
                pItem.commitRole = core::CommitCandidateRole::English;
                pItem.origins = {pItem.source};
                pItem.consumedInputBytes = rawInput.size();
                englishItems.push_back(std::move(pItem));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           englishItems.size() - 1, MoveReason::EnglishPreferOverChinese);
            }

            for (auto &m : macros) {
                englishItems.push_back(std::move(m));
                recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                           englishItems.size() - 1, MoveReason::MacroInsertPos);
            }

            mixed.insert(mixed.begin(),
                         std::make_move_iterator(englishItems.begin()),
                         std::make_move_iterator(englishItems.end()));
        } else {
            std::vector<core::CandidateItem> englishItems;
            englishItems.push_back(std::move(rawCandidate));
            recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                       0, MoveReason::EnglishPreferOverChinese);
            for (auto &m : macros) {
                englishItems.push_back(std::move(m));
                recordMove(core::CandidateSource::Engine, CompositionStage::MacroInsert,
                           englishItems.size() - 1, MoveReason::MacroInsertPos);
            }
            mixed.insert(mixed.begin(),
                         std::make_move_iterator(englishItems.begin()),
                         std::make_move_iterator(englishItems.end()));
        }
        preedit = std::string(rawInput);
    } else {
        std::vector<std::string> preds;
        std::size_t learnedPredCount = 0;
        for (std::size_t idx = 0; idx < input.englishPredictions.size() && preds.size() < 3; ++idx) {
            const auto &p = input.englishPredictions[idx];
            if (std::find(preds.begin(), preds.end(), p) == preds.end()) {
                if (idx < input.learnedEnglishCount) {
                    ++learnedPredCount;
                }
                preds.push_back(p);
            }
        }
        if (!preds.empty()) {
            const bool protectSingleSyllableHomophones =
                !hasUppercase &&
                rawInput.find('\'') == std::string::npos &&
                rawInput.size() <= 3 &&
                core::PinyinMatchPolicy::isStandardSyllable(rawInput) &&
                hasExactFullPinyinMatch && isTopChineseLexical;
            auto insertPos = mixed.empty() ? mixed.begin() : std::next(mixed.begin());
            for (std::size_t pIdx = 0; pIdx < preds.size(); ++pIdx) {
                const auto &p = preds[pIdx];
                core::CandidateItem predictedCandidate;
                predictedCandidate.text = p;
                predictedCandidate.fullPinyin = p;
                predictedCandidate.source = core::CandidateSource::Raw;
                predictedCandidate.commitRole = core::CommitCandidateRole::English;
                predictedCandidate.origins = {predictedCandidate.source};
                predictedCandidate.consumedInputBytes = rawInput.size();

                if (protectSingleSyllableHomophones && pIdx >= 1) {
                    const auto tailIndex = std::min<std::size_t>(mixed.size(), 5);
                    insertPos = mixed.begin() + static_cast<std::ptrdiff_t>(tailIndex);
                }
                const auto idx = static_cast<std::size_t>(std::distance(mixed.begin(), insertPos));
                insertPos = std::next(mixed.insert(insertPos, std::move(predictedCandidate)));
                recordMove(core::CandidateSource::Raw, CompositionStage::EnglishAndRawPlacement,
                           idx, MoveReason::EnglishNormalPlacement);
            }
        }

        bool rawMatchesPred = false;
        for (const auto &p : preds) {
            if (p == rawInput) {
                rawMatchesPred = true;
                break;
            }
        }
        if (!rawMatchesPred) {
            if (isEnglish || !preds.empty()) {
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
        }
        preedit = std::string(input.segmentedInput);
    }

    // Stage 5: English definition annotation lookup
    if (!mixed.empty()) {
        auto &top = mixed.front();
        if (top.source == core::CandidateSource::Raw &&
            top.text.size() >= 4 &&
            core::EnglishDictionary::isEnglishWord(top.text)) {
            if (input.resolveEnglishDefinition) {
                auto def = input.resolveEnglishDefinition(top.text);
                if (!def.empty()) {
                    top.annotation = std::move(def);
                    recordMove(top.source, CompositionStage::EnglishDefinitionAnnotation, 0,
                               MoveReason::InitialPlacement);
                }
            } else if (input.englishDefinition.has_value() && input.isEnglishWord) {
                top.annotation = *input.englishDefinition;
                recordMove(top.source, CompositionStage::EnglishDefinitionAnnotation, 0,
                           MoveReason::InitialPlacement);
            }
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

    // Explicit user preference is applied once, after suppression and identity
    // merging. It cannot invent a candidate or change its consumption/role.
    if (input.pinnedCandidate && core::PinyinMatchPolicy::canonical(rawInput) ==
            core::PinyinMatchPolicy::canonical(input.pinnedCandidate->input)) {
        const auto &pin = *input.pinnedCandidate;
        const auto found = std::find_if(mixed.begin(), mixed.end(), [&](const auto &item) {
            const auto identity = core::candidateIdentity(item, rawInput.size());
            return item.source != core::CandidateSource::None && item.text == pin.text &&
                core::PinyinMatchPolicy::canonical(item.fullPinyin) == core::PinyinMatchPolicy::canonical(pin.fullPinyin) &&
                identity.consumedInputBytes == rawInput.size() &&
                (identity.role == core::CommitCandidateRole::Chinese || identity.role == core::CommitCandidateRole::UserDictionary);
        });
        if (found != mixed.end()) {
            std::rotate(mixed.begin(), found, std::next(found));
            if (mixed.front().annotation.empty()) mixed.front().annotation = "置顶";
            recordMove(mixed.front().source, CompositionStage::ExplicitPin, 0, MoveReason::ExplicitUserPin);
        }
    }

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

std::vector<core::CandidateItem> appendCandidateBatch(
    std::vector<core::CandidateItem> published,
    std::vector<core::CandidateItem> next, std::size_t inputBytes) {
    std::unordered_set<core::CandidateIdentity> seen;
    seen.reserve(published.size() + next.size());
    for (const auto &item : published) seen.insert(core::candidateIdentity(item, inputBytes));
    for (auto &item : next) {
        if (seen.insert(core::candidateIdentity(item, inputBytes)).second)
            published.push_back(std::move(item));
    }
    return published;
}

} // namespace modernime::pinyin
