#pragma once

#include "modernime/core/candidate_prior.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

inline constexpr double DictionaryPriorWeight = 12.0;
inline constexpr double LearningPriorWeight = 4.0;
inline constexpr double ContextPriorWeight = 14.0;
// Deep enough that a steadily accumulated learning boost can carry a
// frequently selected candidate from the tail of the engine list to the
// front page / rank 0, overcoming system dictionary and native rank.
inline constexpr double LearningPriorCap = 180.0;
inline constexpr double DecoderPriorWeight = 0.25;

inline double computeAdaptiveLearning(double learning_boost, double dormant_factor = 1.0) {
    if (!std::isfinite(learning_boost)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (learning_boost <= 0.0) {
        return std::max(-16.0, LearningPriorWeight * learning_boost);
    }
    // Base linear weight provides immediate moderate boost for initial selections,
    // while the progressive power term activates for established habits (boost > 1.8, ~3+ uses)
    // enabling frequent selections to rise from the engine tail (source_index 50~100+)
    // straight to page 1 / rank 0 without allowing single casual clicks to destabilize
    // high-confidence priors.
    const double linear = LearningPriorWeight * learning_boost;
    double progressive = 0.0;
    if (learning_boost > 1.8) {
        const double excess = learning_boost - 1.8;
        progressive = dormant_factor * 36.0 * std::pow(excess, 1.7);
    }
    return std::min(LearningPriorCap, linear + progressive);
}

struct CandidateScore final {
    std::size_t source_index = 0;
    std::string text;
    std::string full_pinyin;
    float decoder_score = 0.0F;
    double decoder_bonus = 0.0;
    int match_priority = 0;
    double learning_boost = 0.0;
    double stability_bonus = 0.0;
    double dictionary_bonus = 0.0;
    double context_bonus = 0.0;
    double unigram_anchor = 0.0;
    std::int64_t last_selected_ms = 0;
    double dormant_factor = 1.0;
    bool is_rare = false;
    bool is_level1 = false;
    bool is_lexical = false;
    double recent_choice_bonus = 0.0;
    std::size_t phrase_length = 0;

    double base_prior() const {
        double score = static_cast<double>(source_index) -
               DictionaryPriorWeight * dictionary_bonus -
               ContextPriorWeight * context_bonus -
               DecoderPriorWeight * decoder_bonus - unigram_anchor;
        if (!is_rare && is_level1) {
            score -= 2.0;
        }
        if (is_lexical) {
            score -= 3.0;
            if (source_index == 0) {
                score -= 2.0;
            }
        } else if (phrase_length >= 2 && context_bonus <= 0.0 && recent_choice_bonus <= 0.0) {
            score += 3.0;
        }
        return score;
    }

    double rare_penalty() const { return is_rare && learning_boost < 1.8 ? 100.0 : 0.0; }

    // Keep the established trusted-abbreviation admission boundary separate
    // from the bounded preference cost used within an admitted group.
    double abbreviation_admission_score() const {
        return base_prior() - computeAdaptiveLearning(learning_boost, dormant_factor) -
               stability_bonus + rare_penalty();
    }

    double final_score() const {
        return boundedCandidatePrior(base_prior()) -
               computeAdaptiveLearning(learning_boost, dormant_factor) -
               stability_bonus - recent_choice_bonus + rare_penalty();
    }
};

class CandidateRanker final {
public:
    static std::vector<std::size_t>
    rank(std::string_view userInput, std::vector<CandidateScore> &candidates,
         const std::vector<std::string> &previousOrder = {});
};

} // namespace modernime::core
