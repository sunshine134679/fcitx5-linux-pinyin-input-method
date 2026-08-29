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
// front page, while still bounded so learning never overrides match
// priority (input coverage) itself.
inline constexpr double LearningPriorCap = 16.0;
inline constexpr double DecoderPriorWeight = 0.25;

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

    double final_score() const {
        const double adaptive_learning = std::clamp(
            LearningPriorWeight * learning_boost, -LearningPriorCap,
            LearningPriorCap);
        return static_cast<double>(source_index) - adaptive_learning -
               stability_bonus - DictionaryPriorWeight * dictionary_bonus -
               context_bonus - DecoderPriorWeight * decoder_bonus;
    }
};

class CandidateRanker final {
public:
    static std::vector<std::size_t>
    rank(std::string_view userInput, std::vector<CandidateScore> &candidates,
         const std::vector<std::string> &previousOrder = {});
};

} // namespace modernime::core
