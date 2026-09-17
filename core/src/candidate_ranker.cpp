#include "modernime/core/candidate_ranker.h"

#include "modernime/core/candidate_model.h"
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
        candidate.decoder_bonus = 0.0;
    }
    float minimumDecoderScore = std::numeric_limits<float>::max();
    float maximumDecoderScore = std::numeric_limits<float>::lowest();
    for (const auto &candidate : candidates) {
        if (!std::isfinite(candidate.decoder_score)) {
            continue;
        }
        minimumDecoderScore =
            std::min(minimumDecoderScore, candidate.decoder_score);
        maximumDecoderScore =
            std::max(maximumDecoderScore, candidate.decoder_score);
    }
    if (minimumDecoderScore < maximumDecoderScore) {
        const auto range = static_cast<double>(maximumDecoderScore) -
                           static_cast<double>(minimumDecoderScore);
        for (auto &candidate : candidates) {
            if (!std::isfinite(candidate.decoder_score)) {
                continue;
            }
            candidate.decoder_bonus =
                (static_cast<double>(maximumDecoderScore) -
                 static_cast<double>(candidate.decoder_score)) /
                range;
        }
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
                candidateOrderKey(candidate.text, candidate.full_pinyin));
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
    std::stable_sort(order.begin(), order.end(), [&candidates, userInput](std::size_t left,
                                                                std::size_t right) {
        const auto &a = candidates[left];
        const auto &b = candidates[right];
        if (a.match_priority != b.match_priority) {
            return a.match_priority > b.match_priority;
        }
        const bool aTrusted = a.match_priority == 1 &&
            PinyinMatchPolicy::trustedShortAbbreviationMatch(userInput, a.full_pinyin, a.text);
        const bool bTrusted = b.match_priority == 1 &&
            PinyinMatchPolicy::trustedShortAbbreviationMatch(userInput, b.full_pinyin, b.text);
        if (aTrusted != bTrusted) {
            const auto &trusted = aTrusted ? a : b;
            const auto &other = aTrusted ? b : a;
            if (other.learning_boost >= 2.5 &&
                other.final_score() < trusted.final_score()) {
                return a.final_score() < b.final_score();
            }
            return aTrusted;
        }
        if (a.final_score() != b.final_score()) {
            return a.final_score() < b.final_score();
        }
        return a.source_index < b.source_index;
    });
    return order;
}

} // namespace modernime::core
