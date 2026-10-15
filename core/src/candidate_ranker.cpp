#include "modernime/core/candidate_ranker.h"

#include "modernime/core/candidate_model.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <numeric>
#include <unordered_map>

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
    std::unordered_map<int, std::int64_t> latestByPriority;
    for (const auto &candidate : candidates) {
        auto &latest = latestByPriority[candidate.match_priority];
        latest = std::max(latest, candidate.last_selected_ms);
    }
    constexpr double kDormantHalfLifeMs = 12.0 * 3600.0 * 1000.0;
    constexpr double kDormantTauMs = kDormantHalfLifeMs / 0.6931471805599453;
    for (auto &candidate : candidates) {
        const auto latest = latestByPriority[candidate.match_priority];
        if (latest > 0 && candidate.last_selected_ms > 0 && candidate.last_selected_ms < latest) {
            const auto ageSinceLatest = latest - candidate.last_selected_ms;
            candidate.dormant_factor =
                std::exp(-static_cast<double>(ageSinceLatest) / kDormantTauMs);
        } else {
            candidate.dormant_factor = 1.0;
        }
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
        std::unordered_map<std::string_view, std::size_t> previousIndex;
        previousIndex.reserve(previousOrder.size());
        for (std::size_t i = 0; i < previousOrder.size(); ++i) {
            previousIndex.emplace(previousOrder[i], i);
        }
        for (auto &candidate : candidates) {
            const auto it = previousIndex.find(
                candidateOrderKey(candidate.text, candidate.full_pinyin));
            if (it == previousIndex.end()) {
                continue;
            }
            const auto distance = baseCost(candidate) - best;
            if (distance > 0.0 && distance <= 0.5) {
                const auto position = it->second;
                candidate.stability_bonus =
                    std::min(0.40, 0.40 / static_cast<double>(position + 1));
            }
        }
    }
    // Decide abbreviation promotion once for the entire candidate set. A
    // pair-dependent learning exemption can create a cycle between a trusted
    // phrase, a learned alternative, and an unlearned native leader.
    std::vector<bool> abbreviationPromoted(candidates.size(), false);
    double bestTrustedScore = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto &candidate = candidates[index];
        if (candidate.match_priority == 1 &&
            PinyinMatchPolicy::trustedShortAbbreviationMatch(
                userInput, candidate.full_pinyin, candidate.text)) {
            abbreviationPromoted[index] = true;
            bestTrustedScore = std::min(bestTrustedScore, candidate.abbreviation_admission_score());
        }
    }
    if (std::isfinite(bestTrustedScore)) {
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index];
            if (candidate.match_priority == 1 && candidate.learning_boost >= 2.5 &&
                candidate.abbreviation_admission_score() < bestTrustedScore) {
                abbreviationPromoted[index] = true;
            }
        }
    } else {
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index];
            if (candidate.match_priority == 1 && candidate.learning_boost >= 0.8) {
                abbreviationPromoted[index] = true;
            }
        }
    }
    std::vector<double> preferenceCosts;
    preferenceCosts.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        // Unadmitted alternatives must not use preference normalization to
        // bypass the existing trusted-abbreviation boundary.
        const auto &candidate = candidates[i];
        preferenceCosts.push_back(std::isfinite(bestTrustedScore) &&
                candidate.match_priority == 1 && !abbreviationPromoted[i]
            ? candidate.abbreviation_admission_score() : candidate.final_score());
    }
    std::stable_sort(order.begin(), order.end(),
                     [&candidates, &abbreviationPromoted, &preferenceCosts](std::size_t left,
                                                        std::size_t right) -> bool {
        const auto &a = candidates[left];
        const auto &b = candidates[right];
        if (a.match_priority != b.match_priority) {
            return a.match_priority > b.match_priority;
        }
        if (abbreviationPromoted[left] != abbreviationPromoted[right]) {
            return abbreviationPromoted[left];
        }
        if (preferenceCosts[left] != preferenceCosts[right]) {
            return preferenceCosts[left] < preferenceCosts[right];
        }
        if (a.source_index != b.source_index) {
            return a.source_index < b.source_index;
        }
        return candidateOrderKey(a.text, a.full_pinyin) <
               candidateOrderKey(b.text, b.full_pinyin);
    });
    return order;
}

} // namespace modernime::core
