#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/candidate_features.h"

#include "modernime/core/candidate_model.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/pinyin_match.h"

#include <unordered_set>

#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>

#include <algorithm>
#include <string_view>

namespace modernime::pinyin {
CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter,
    const std::vector<std::string> &previousOrder,
    const libime::PinyinContext *typoContext, const RecentChoices *recentChoices) {
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
    result.scored.reserve(scoreLimit + 16);
    std::unordered_set<std::string> seenKeys;
    seenKeys.reserve(scoreLimit + 16);
    for (std::size_t index = 0; index < scoreLimit; ++index) {
        core::CandidateScore candidate;
        candidate.source_index = index;
        candidate.text = nativeCandidates[index].toString();
        try {
            candidate.full_pinyin = context.candidateFullPinyin(index);
        } catch (const std::exception &) {
            candidate.full_pinyin.clear();
        }
        candidate.decoder_score = nativeCandidates[index].score();
        if (index == 0) candidate.unigram_anchor = 3.0;
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
                        if (index == 0) {
                            existing.unigram_anchor = 3.0;
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
            if (index == 0) candidate.unigram_anchor = 3.0;
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
