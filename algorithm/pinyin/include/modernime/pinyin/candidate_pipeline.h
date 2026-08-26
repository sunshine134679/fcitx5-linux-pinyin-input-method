#pragma once

#include "modernime/core/candidate_ranker.h"

#include <cstddef>
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
    const libime::PinyinDictionary &dictionary);

} // namespace modernime::pinyin
