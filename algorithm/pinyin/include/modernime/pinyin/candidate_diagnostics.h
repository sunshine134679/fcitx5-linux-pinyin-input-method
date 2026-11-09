#pragma once

#include "modernime/core/candidate_ranker.h"
#include "modernime/pinyin/candidate_composition.h"

namespace modernime::pinyin {

// Opt-in evidence for the current recall batch, before and after composition.
// Never enabled by the input method; the caller owns exporting any input text.
struct PinyinDiagnostics final {
    std::uint64_t generation = 0;
    std::int64_t timestampMs = 0;
    std::size_t nativeOffset = 0;
    std::string segmentedInput;
    std::vector<core::CandidateScore> scored;
    std::vector<std::size_t> order;
    std::vector<CandidateMoveRecord> moves;
};

} // namespace modernime::pinyin
