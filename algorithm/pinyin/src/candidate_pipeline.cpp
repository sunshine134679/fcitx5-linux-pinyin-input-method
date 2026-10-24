#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/candidate_features.h"

#include "modernime/core/candidate_model.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/pinyin_match.h"

#include <unordered_set>

#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace modernime::pinyin {
CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter,
    const std::vector<std::string> &previousOrder,
    const libime::PinyinContext *typoContext, const RecentChoices *recentChoices,
    const libime::PinyinContext *variantContext) {
    CandidatePipelineResult result;
    const auto &nativeCandidates = context.candidates();
    // libime 的候选已按解码代价升序排列。简拼输入（如 yyds）会展开出
    // 数千个候选，全部进入学习 boost + 词典加分评分循环会让单键耗时
    // 超过 100ms；用户翻页永远看不到 128 名以外的候选，这里按 libime
    // 原生顺序截断头部再评分，保证评分管线只在有限集合上工作。
    constexpr std::size_t kMaxScoredCandidates = 128;
    const std::size_t scoreLimit =
        nativeCandidates.size() < kMaxScoredCandidates
            ? nativeCandidates.size()
            : kMaxScoredCandidates;

    struct NativeCandidateEntry {
        std::string text;
        std::string full_pinyin;
        float decoder_score = 0.0F;
        int match_priority = 0;
    };
    std::vector<NativeCandidateEntry> nativeEntries;
    nativeEntries.reserve(scoreLimit);
    for (std::size_t index = 0; index < scoreLimit; ++index) {
        NativeCandidateEntry entry;
        entry.text = nativeCandidates[index].toString();
        try {
            entry.full_pinyin = context.candidateFullPinyin(index);
        } catch (const std::exception &) {
            entry.full_pinyin.clear();
        }
        entry.decoder_score = nativeCandidates[index].score();
        entry.match_priority =
            core::PinyinMatchPolicy::priority(context.userInput(), entry.full_pinyin);
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
            std::stable_sort(nativeEntries.begin() + i, nativeEntries.begin() + j,
                             [](const NativeCandidateEntry &a, const NativeCandidateEntry &b) {
                                 if (a.match_priority != b.match_priority) {
                                     return a.match_priority > b.match_priority;
                                 }
                                 return a.text < b.text;
                             });
        }
        i = j;
    }

    result.scored.reserve(scoreLimit + 16);
    std::unordered_set<std::string> seenKeys;
    seenKeys.reserve(scoreLimit + 16);
    for (std::size_t index = 0; index < nativeEntries.size(); ++index) {
        auto &entry = nativeEntries[index];
        core::CandidateScore candidate;
        candidate.source_index = index;
        candidate.text = std::move(entry.text);
        candidate.full_pinyin = std::move(entry.full_pinyin);
        candidate.decoder_score = entry.decoder_score;
        if (index == 0) candidate.unigram_anchor = 7.0;
        seenKeys.insert(core::candidateOrderKey(candidate.text, candidate.full_pinyin));
        result.scored.push_back(std::move(candidate));
    }

    if (typoContext != nullptr) {
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
            const auto key = core::candidateOrderKey(text, fullPinyin);
            if (seenKeys.contains(key)) {
                for (auto &existing : result.scored) {
                    if (existing.text == text && existing.full_pinyin == fullPinyin) {
                        if (index < existing.source_index) {
                            existing.source_index = index;
                            existing.decoder_score = typoCandidates[index].score();
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
            // 容错补充候选稍加惩罚（+0.8F 解码代价），避免在原词完全合法时过度喧宾夺主
            candidate.decoder_score = typoCandidates[index].score() + 0.8F;
            result.scored.push_back(std::move(candidate));
        }
    }

    if (variantContext != nullptr) {
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
            const auto key = core::candidateOrderKey(text, fullPinyin);
            if (seenKeys.contains(key)) {
                continue;
            }
            seenKeys.insert(key);
            core::CandidateScore candidate;
            candidate.source_index = index;
            candidate.text = text;
            candidate.full_pinyin = fullPinyin;
            // 近音变体是最低位的补充召回来源：惩罚显著高于纠错候选（+0.8），
            // 且只依赖 F1 的成熟学习提升或既有混排插位获得可见性。
            candidate.decoder_score = variantCandidates[index].score() + 2.0F;
            result.scored.push_back(std::move(candidate));
        }
    }

    if (learning != nullptr) {
        const auto recalled = learning->recallCandidates(
            context.userInput(), nowMs, contextBefore, contextAfter);
        for (const auto &item : recalled) {
            const auto cp = core::CommonCharacters::firstCodepoint(item.phrase);
            if (!((cp >= 0x4E00 && cp <= 0x9FFF) ||
                  (cp >= 0x3400 && cp <= 0x4DBF) ||
                  (cp >= 0x20000 && cp <= 0x2EBEF))) {
                continue;
            }
            const std::string segmentedPinyin =
                core::PinyinMatchPolicy::segmentPinyin(item.pinyin);
            const auto key = core::candidateOrderKey(item.phrase, segmentedPinyin);
            if (seenKeys.contains(key)) continue;
            seenKeys.insert(key);
            core::CandidateScore candidate;
            // No native position exists for a recall-only candidate. Use a
            // neutral prior, never the snapshot's shard/enumeration position.
            candidate.source_index = 0;
            candidate.text = item.phrase;
            candidate.full_pinyin = segmentedPinyin;
            result.scored.push_back(std::move(candidate));
        }
    }

    const CandidateFeatureContext request{context.userInput(), learning, recentChoices,
                                          nowMs, contextBefore, contextAfter};
    for (auto &candidate : result.scored) {
        populateCandidateFeatures(candidate, dictionary, request);
    }
    result.order = core::CandidateRanker::rank(
        context.userInput(), result.scored, previousOrder);
    return result;
}

} // namespace modernime::pinyin
