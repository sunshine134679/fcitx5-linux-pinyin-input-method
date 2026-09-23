#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

inline constexpr double DictionaryPriorWeight = 12.0;
inline constexpr double LearningPriorWeight = 4.0;
// Deep enough that a steadily accumulated learning boost can carry a
// frequently selected candidate from the tail of the engine list to the
// front page / rank 0, overcoming system dictionary and native rank.
inline constexpr double LearningPriorCap = 180.0;
inline constexpr double DecoderPriorWeight = 0.25;

inline double computeAdaptiveLearning(double learning_boost) {
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
        progressive = 36.0 * std::pow(excess, 1.7);
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

    double final_score() const {
        return static_cast<double>(source_index) -
               computeAdaptiveLearning(learning_boost) - stability_bonus -
               DictionaryPriorWeight * dictionary_bonus - context_bonus -
               DecoderPriorWeight * decoder_bonus - unigram_anchor;
    }
};

class CandidateRanker final {
public:
    static std::vector<std::size_t>
    rank(std::string_view userInput, std::vector<CandidateScore> &candidates,
         const std::vector<std::string> &previousOrder = {});
};

} // namespace modernime::core
