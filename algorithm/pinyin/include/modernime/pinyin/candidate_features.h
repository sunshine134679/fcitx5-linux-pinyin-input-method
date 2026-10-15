#pragma once
#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_snapshot.h"
#include <cstdint>
#include <string_view>
namespace libime { class PinyinDictionary; }
namespace modernime::pinyin {
class RecentChoices;
// Immutable inputs for one ranking request; no storage or composition policy.
struct CandidateFeatureContext final {
    std::string_view input;
    const core::LearningSnapshot *learning = nullptr;
    const RecentChoices *recentChoices = nullptr;
    std::int64_t nowMs = 0;
    std::string_view before;
    std::string_view after;
};
void populateCandidateFeatures(core::CandidateScore &candidate,
                               const libime::PinyinDictionary &dictionary,
                               const CandidateFeatureContext &request);
} // namespace modernime::pinyin
