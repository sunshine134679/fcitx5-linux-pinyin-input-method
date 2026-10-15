#pragma once

#include "modernime/core/candidate_model.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace modernime::pinyin {

std::vector<core::CandidateItem> mixCandidateItems(
    const std::vector<core::CandidateItem> &fullCandidates,
    const std::vector<core::CandidateItem> &partialCandidates,
    const core::CandidateItem *bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds,
    std::string_view rawPinyin,
    const std::vector<core::CandidateItem> *prefixEvidence = nullptr);

} // namespace modernime::pinyin
