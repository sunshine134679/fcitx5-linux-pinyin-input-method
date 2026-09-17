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
    const libime::PinyinContext *typoContext = nullptr);

} // namespace modernime::pinyin
