#pragma once

#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace libime {
class PinyinContext;
class PinyinDictionary;
} // namespace libime

namespace modernime::pinyin {
class RecentChoices;

struct CandidatePipelineResult final {
    std::vector<core::CandidateScore> scored;
    std::vector<std::size_t> order;
};

CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning = nullptr,
    std::int64_t nowMs = 0,
    std::string_view contextBefore = {},
    std::string_view contextAfter = {},
    const std::vector<std::string> &previousOrder = {},
    const libime::PinyinContext *typoContext = nullptr,
    const RecentChoices *recentChoices = nullptr,
    // 容错变体上下文（2026-10-24 容错专项）：由 rhymeVariantInput 派生的近音
    // 变体解码（如 kazhu -> kazhe）。变体候选带 +2.0 解码惩罚，作为低位补充
    // 召回来源，不改写主解码排序。
    const libime::PinyinContext *variantContext = nullptr);

} // namespace modernime::pinyin
