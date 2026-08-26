#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

inline constexpr double DictionaryPriorWeight = 12.0;

struct CandidateScore final {
    std::size_t source_index = 0;
    std::string text;
    std::string full_pinyin;
    float decoder_score = 0.0F;
    int match_priority = 0;
    double learning_boost = 0.0;
    double stability_bonus = 0.0;
    double dictionary_bonus = 0.0;
    double context_bonus = 0.0;

    double final_score() const {
        return static_cast<double>(source_index) - learning_boost -
               stability_bonus - DictionaryPriorWeight * dictionary_bonus -
               context_bonus;
    }
};

class CandidateRanker final {
public:
    static std::vector<std::size_t>
    rank(std::string_view userInput, std::vector<CandidateScore> &candidates,
         const std::vector<std::string> &previousOrder = {});
};

} // namespace modernime::core
