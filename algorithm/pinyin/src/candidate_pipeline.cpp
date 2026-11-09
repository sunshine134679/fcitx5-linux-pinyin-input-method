#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/candidate_features.h"
#include "modernime/pinyin/shuangpin_input.h"
#include "modernime/pinyin/context_decoder.h"

#include "modernime/core/candidate_model.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/pinyin_match.h"

#include <unordered_set>

#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace modernime::pinyin {
namespace {
// 将解码输入上的边界映射到原始击键。纠错可能增删字母/隔音符；只接受
// 未改前缀或未改后缀确定的边界。两种证据冲突（如重复字母）时拒绝该
// 补充部分候选，不能把不明边界默认为消费整串。
std::optional<std::size_t> inputBoundary(
    const libime::PinyinContext &decoded, const libime::SentenceResult &candidate,
    std::string_view rawInput) {
    const auto &sentence = candidate.sentence();
    if (sentence.empty() || sentence.back() == nullptr ||
        sentence.back()->to() == nullptr) return std::nullopt;
    const auto decodedInput = decoded.userInput();
    auto end = sentence.back()->to()->index();
    if (end == 0 || end > decodedInput.size()) return std::nullopt;
    while (end < decodedInput.size() && decodedInput[end] == '\'') ++end;
    if (decodedInput == rawInput) return end;
    const auto from = core::PinyinMatchPolicy::canonical(decodedInput);
    const auto to = core::PinyinMatchPolicy::canonical(rawInput);
    const auto prefix = core::PinyinMatchPolicy::canonical(decodedInput.substr(0, end));
    const auto split = prefix.size();
    std::optional<std::size_t> letterEnd;
    if (split == from.size()) {
        letterEnd = to.size();
    } else {
        if (to.starts_with(prefix)) letterEnd = split;
        const auto suffix = std::string_view(from).substr(split);
        if (to.ends_with(suffix)) {
            const auto suffixEnd = to.size() - suffix.size();
            if (letterEnd && *letterEnd != suffixEnd) return std::nullopt;
            letterEnd = suffixEnd;
        }
    }
    if (!letterEnd || *letterEnd == 0) return std::nullopt;
    std::size_t letters = 0;
    for (std::size_t i = 0; i < rawInput.size(); ++i) {
        if (rawInput[i] != '\'' && rawInput[i] != ',') ++letters;
        if (letters != *letterEnd) continue;
        auto rawEnd = i + 1;
        while (rawEnd < rawInput.size() &&
               (rawInput[rawEnd] == '\'' || rawInput[rawEnd] == ',')) ++rawEnd;
        return rawEnd;
    }
    return std::nullopt;
}

std::string spanKey(std::string_view text, std::string_view pinyin,
                    std::size_t consumed) {
    auto key = core::candidateOrderKey(text, pinyin);
    key.push_back('\x1e');
    key += std::to_string(consumed);
    return key;
}
} // namespace

CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter,
    const std::vector<std::string> &previousOrder,
    const libime::PinyinContext *typoContext, const RecentChoices *recentChoices,
    const libime::PinyinContext *variantContext, const ShuangpinInput *shuangpin,
    std::size_t nativeOffset, const ContextDecoder *contextDecoder) {
    CandidatePipelineResult result;
    const auto &nativeCandidates = context.candidates();
    // libime 保留 N-best 整句在前，后续候选按解码分数降序排列；高分更优。
    // 简拼输入（如 yyds）会展开出
    // 数千个候选，全部进入学习 boost + 词典加分评分循环会让单键耗时
    // 超过 100ms；每批只处理128项，后续批次由翻页按需请求。
    constexpr std::size_t kMaxScoredCandidates = 128;
    const auto begin = std::min(nativeOffset, nativeCandidates.size());
    const auto scoreLimit = std::min(kMaxScoredCandidates, nativeCandidates.size() - begin);
    result.nextNativeCandidate = begin + scoreLimit;
    result.hasMoreCandidates = result.nextNativeCandidate < nativeCandidates.size();

    struct NativeCandidateEntry {
        std::string text;
        std::string full_pinyin;
        float decoder_score = 0.0F;
        double language_context_bonus = 0.0;
        int match_priority = 0;
        std::size_t consumed = 0;
        std::size_t source_index = 0;
    };
    std::vector<NativeCandidateEntry> nativeEntries;
    nativeEntries.reserve(scoreLimit);
    for (std::size_t index = begin; index < result.nextNativeCandidate; ++index) {
        NativeCandidateEntry entry;
        entry.source_index = index;
        entry.text = nativeCandidates[index].toString();
        try {
            entry.full_pinyin = context.candidateFullPinyin(index);
        } catch (const std::exception &) {
            entry.full_pinyin.clear();
        }
        const auto consumed = inputBoundary(context, nativeCandidates[index], context.userInput());
        if (!consumed) continue;
        entry.consumed = *consumed;
        entry.decoder_score = nativeCandidates[index].score();
        if (contextDecoder) entry.language_context_bonus = contextDecoder->gain(nativeCandidates[index]);
        if (shuangpin != nullptr) {
            const auto match = shuangpin->match(entry.full_pinyin);
            if (!match || match->consumed != entry.consumed) continue;
            entry.match_priority = match->priority;
        } else {
            entry.match_priority = core::PinyinMatchPolicy::priority(context.userInput(), entry.full_pinyin);
        }
        nativeEntries.push_back(std::move(entry));
    }

    // 对解码分数完全相等（差值小于 10^-6）且匹配优先级相同的同等级候选，
    // 使用词汇文本的 Unicode 码点顺序进行确定性决胜（Tie-Breaking），消除编辑回退抖动
    for (std::size_t i = 0; i < nativeEntries.size();) {
        std::size_t j = i + 1;
        while (j < nativeEntries.size() &&
               std::abs(nativeEntries[j].decoder_score - nativeEntries[i].decoder_score) < 1e-6F) {
            ++j;
        }
        if (j > i + 1) {
            std::vector<std::size_t> priorSlots;
            for (auto n = i; n < j; ++n) priorSlots.push_back(nativeEntries[n].source_index);
            std::stable_sort(nativeEntries.begin() + i, nativeEntries.begin() + j,
                             [](const NativeCandidateEntry &a, const NativeCandidateEntry &b) {
                                 if (a.match_priority != b.match_priority) {
                                     return a.match_priority > b.match_priority;
                                 }
                                 return a.text < b.text;
                             });
            // 保持既有同分仲裁的确定性位置先验；跳过无效格点不压缩原槽位，
            // 且消费事实已经独立保存，绝不用这个先验位置反查原生格点。
            for (auto n = i; n < j; ++n) nativeEntries[n].source_index = priorSlots[n - i];
        }
        i = j;
    }

    result.scored.reserve(scoreLimit + 16);
    std::unordered_set<std::string> seenKeys;
    seenKeys.reserve(scoreLimit + 16);
    std::unordered_set<std::string> seenWords;
    for (std::size_t index = 0; index < nativeEntries.size(); ++index) {
        auto &entry = nativeEntries[index];
        core::CandidateScore candidate;
        candidate.source_index = entry.source_index;
        candidate.text = std::move(entry.text);
        candidate.full_pinyin = std::move(entry.full_pinyin);
        candidate.decoder_score = entry.decoder_score;
        candidate.language_context_bonus = entry.language_context_bonus;
        if (entry.source_index == 0) candidate.unigram_anchor = 7.0;
        candidate.consumed_input_bytes = entry.consumed;
        if (shuangpin != nullptr) candidate.decoded_match_priority = entry.match_priority;
        seenKeys.insert(spanKey(candidate.text, candidate.full_pinyin, entry.consumed));
        seenWords.insert(core::candidateOrderKey(candidate.text, candidate.full_pinyin));
        result.scored.push_back(std::move(candidate));
    }

    if (nativeOffset == 0 && typoContext != nullptr) {
        const auto &typoCandidates = typoContext->candidates();
        const std::size_t typoLimit =
            std::min<std::size_t>(typoCandidates.size(), 16);
        for (std::size_t index = 0; index < typoLimit; ++index) {
            const auto text = typoCandidates[index].toString();
            std::string fullPinyin;
            try {
                fullPinyin = typoContext->candidateFullPinyin(index);
            } catch (const std::exception &) {
                fullPinyin.clear();
            }
            if (fullPinyin.empty()) {
                continue;
            }
            const auto consumed = inputBoundary(*typoContext, typoCandidates[index], context.userInput());
            if (!consumed) continue;
            const float adjustedScore = typoCandidates[index].score() - 0.8F;
            const auto key = spanKey(text, fullPinyin, *consumed);
            if (seenKeys.contains(key)) {
                for (auto &existing : result.scored) {
                    if (existing.text == text && existing.full_pinyin == fullPinyin &&
                        existing.consumed_input_bytes == consumed) {
                        existing.source_index = std::min(index, existing.source_index);
                        // Source position and decoder confidence are separate
                        // evidence. A duplicate must not erase a stronger score.
                        if (std::isfinite(adjustedScore) &&
                            (!existing.decoder_score || !std::isfinite(*existing.decoder_score) ||
                             adjustedScore > *existing.decoder_score)) {
                            existing.decoder_score = adjustedScore;
                        }
                        break;
                    }
                }
                continue;
            }
            seenKeys.insert(key);
            core::CandidateScore candidate;
            candidate.source_index = index;
            candidate.text = text;
            candidate.full_pinyin = fullPinyin;
            candidate.consumed_input_bytes = *consumed;
            seenWords.insert(core::candidateOrderKey(text, fullPinyin));
            // 分数越高越优，容错补充扣除 0.8，避免把纠错惩罚变成奖励。
            candidate.decoder_score = adjustedScore;
            if (contextDecoder) candidate.language_context_bonus = contextDecoder->gain(typoCandidates[index]);
            result.scored.push_back(std::move(candidate));
        }
    }

    if (nativeOffset == 0 && variantContext != nullptr) {
        const auto &variantCandidates = variantContext->candidates();
        const std::size_t variantLimit =
            std::min<std::size_t>(variantCandidates.size(), 16);
        for (std::size_t index = 0; index < variantLimit; ++index) {
            const auto text = variantCandidates[index].toString();
            std::string fullPinyin;
            try {
                fullPinyin = variantContext->candidateFullPinyin(index);
            } catch (const std::exception &) {
                fullPinyin.clear();
            }
            if (fullPinyin.empty()) {
                continue;
            }
            const auto consumed = inputBoundary(*variantContext, variantCandidates[index], context.userInput());
            if (!consumed) continue;
            const auto key = spanKey(text, fullPinyin, *consumed);
            if (seenKeys.contains(key)) {
                continue;
            }
            seenKeys.insert(key);
            core::CandidateScore candidate;
            candidate.source_index = index;
            candidate.text = text;
            candidate.full_pinyin = fullPinyin;
            candidate.consumed_input_bytes = *consumed;
            seenWords.insert(core::candidateOrderKey(text, fullPinyin));
            // 近音变体是最低位的补充召回来源：扣分幅度高于纠错候选（0.8），
            // 且只依赖 F1 的成熟学习提升或既有混排插位获得可见性。
            candidate.decoder_score = variantCandidates[index].score() - 2.0F;
            if (contextDecoder) candidate.language_context_bonus = contextDecoder->gain(variantCandidates[index]);
            result.scored.push_back(std::move(candidate));
        }
    }

    if (nativeOffset == 0 && contextDecoder) {
        for (auto candidate : contextDecoder->candidates()) {
            if (shuangpin) {
                const auto match = shuangpin->match(candidate.full_pinyin);
                if (!match || match->consumed != candidate.consumed_input_bytes) continue;
                candidate.decoded_match_priority = match->priority;
            }
            const auto key = spanKey(candidate.text, candidate.full_pinyin, *candidate.consumed_input_bytes);
            if (!seenKeys.insert(key).second) {
                for (auto &existing : result.scored)
                    if (spanKey(existing.text, existing.full_pinyin, existing.consumed_input_bytes.value_or(0)) == key)
                        existing.language_context_bonus = std::max(existing.language_context_bonus, candidate.language_context_bonus);
                continue;
            }
            seenWords.insert(core::candidateOrderKey(candidate.text, candidate.full_pinyin));
            result.scored.push_back(std::move(candidate));
        }
    }
    const auto recallInputs = shuangpin != nullptr ? shuangpin->recallInputs()
        : std::vector<std::string>{context.userInput()};
    if (nativeOffset == 0 && learning != nullptr) {
      for (const auto &recallInput : recallInputs) {
        const auto recalled = learning->recallCandidates(
            recallInput, nowMs, contextBefore, contextAfter);
        for (const auto &item : recalled) {
            const auto cp = core::CommonCharacters::firstCodepoint(item.phrase);
            if (!((cp >= 0x4E00 && cp <= 0x9FFF) ||
                  (cp >= 0x3400 && cp <= 0x4DBF) ||
                  (cp >= 0x20000 && cp <= 0x2EBEF))) {
                continue;
            }
            const std::string segmentedPinyin =
                core::PinyinMatchPolicy::segmentPinyin(item.pinyin);
            const auto decodedMatch = shuangpin != nullptr
                ? shuangpin->match(segmentedPinyin) : std::optional<ShuangpinInput::Match>{};
            if (shuangpin != nullptr && !decodedMatch) continue;
            const auto key = core::candidateOrderKey(item.phrase, segmentedPinyin);
            if (!seenWords.insert(key).second) continue;
            core::CandidateScore candidate;
            // No native position exists for a recall-only candidate. Use a
            // neutral prior, never the snapshot's shard/enumeration position.
            candidate.source_index = 0;
            candidate.text = item.phrase;
            candidate.full_pinyin = segmentedPinyin;
            if (decodedMatch) {
                candidate.consumed_input_bytes = decodedMatch->consumed;
                candidate.decoded_match_priority = decodedMatch->priority;
            }
            result.scored.push_back(std::move(candidate));
        }
      }
    }

    const CandidateFeatureContext request{context.userInput(), learning, recentChoices,
                                          nowMs, contextBefore, contextAfter};
    for (auto &candidate : result.scored) {
        populateCandidateFeatures(candidate, dictionary, request);
    }
    result.order = core::CandidateRanker::rank(
        shuangpin != nullptr && !recallInputs.empty() ? recallInputs.front() : context.userInput(),
        result.scored, previousOrder);
    return result;
}

} // namespace modernime::pinyin
