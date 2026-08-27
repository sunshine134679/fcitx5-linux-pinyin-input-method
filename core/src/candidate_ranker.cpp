#include "modernime/core/candidate_ranker.h"

#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <numeric>

namespace modernime::core {

std::vector<std::size_t>
CandidateRanker::rank(std::string_view userInput,
                     std::vector<CandidateScore> &candidates,
                     const std::vector<std::string> &previousOrder) {
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    for (auto &candidate : candidates) {
        candidate.match_priority =
            PinyinMatchPolicy::priority(userInput, candidate.full_pinyin);
        candidate.stability_bonus = 0.0;
    }
    if (!previousOrder.empty() && !candidates.empty()) {
        const auto baseCost = [](const CandidateScore &candidate) {
            return candidate.final_score();
        };
        double best = baseCost(candidates.front());
        for (const auto &candidate : candidates) {
            best = std::min(best, baseCost(candidate));
        }
        for (auto &candidate : candidates) {
            const auto previous = std::find(
                previousOrder.begin(), previousOrder.end(),
                candidate.text + '\x1f' + candidate.full_pinyin);
            if (previous == previousOrder.end()) {
                continue;
            }
            const auto distance = baseCost(candidate) - best;
            if (distance > 0.0 && distance <= 0.5) {
                const auto position = static_cast<std::size_t>(
                    std::distance(previousOrder.begin(), previous));
                candidate.stability_bonus =
                    std::min(0.40, 0.40 / static_cast<double>(position + 1));
            }
        }
    }
    std::stable_sort(order.begin(), order.end(), [&candidates](std::size_t left,
                                                                std::size_t right) {
        const auto &a = candidates[left];
        const auto &b = candidates[right];
        if (a.match_priority != b.match_priority) {
            return a.match_priority > b.match_priority;
        }
        if (a.final_score() != b.final_score()) {
            return a.final_score() < b.final_score();
        }
        return a.source_index < b.source_index;
    });
    return order;
}

} // namespace modernime::core
