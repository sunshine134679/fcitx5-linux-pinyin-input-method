#pragma once

#include "modernime/core/candidate_model.h"

namespace modernime::core { class LearningSnapshot; }
namespace modernime::pinyin {
class RecentChoices;

// Identity/feature stage: parent segmentation + the prefix's OWN evidence.
// Does not assign final positions or mutate learning/session state.
std::vector<core::CandidateItem> buildPrefixEvidence(
    const core::CandidateItem &best, const std::vector<std::size_t> &ends,
    std::string_view input, const std::vector<core::CandidateItem> &partial = {},
    const core::LearningSnapshot *learning = nullptr, std::int64_t nowMs = 0,
    const RecentChoices *recentChoices = nullptr);

bool isMatchingSentenceHomophone(const core::CandidateItem &candidate,
    const core::CandidateItem &best, const std::vector<std::size_t> &ends);

} // namespace modernime::pinyin
