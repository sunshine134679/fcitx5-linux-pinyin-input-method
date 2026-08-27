#include "modernime/core/candidate_ranker.h"
#include "modernime/core/pinyin_match.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin match test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using modernime::core::CandidateRanker;
    using modernime::core::CandidateScore;
    using modernime::core::PinyinMatchPolicy;

    assertTrue(PinyinMatchPolicy::canonical("Ni'Hao") == "nihao",
               "canonical pinyin removes separators and lowercases");
    assertTrue(PinyinMatchPolicy::abbreviationKey("ni'hao") == "nh",
               "abbreviation uses syllable initials");
    assertTrue(PinyinMatchPolicy::priority("nihao", "ni'hao") == 2,
               "exact match has highest priority");
    assertTrue(PinyinMatchPolicy::priority("ni'hao", "ni'hao") == 2,
               "separated exact input has highest priority");
    assertTrue(PinyinMatchPolicy::priority("nh", "ni'hao") == 1,
               "abbreviation match has secondary priority");
    assertTrue(PinyinMatchPolicy::priority("nihaoma", "ni'hao") == -1,
               "prefix candidate is marked as incomplete");

    std::vector<CandidateScore> candidates{
        {0, "你", "ni", 0.0F}, {1, "你好", "ni'hao", 0.0F}};
    const auto order = CandidateRanker::rank("nihao", candidates);
    assertTrue(order.size() == 2 && order[0] == 1,
               "exact candidate outranks a prefix candidate");
    assertTrue(candidates[1].match_priority == 2,
               "ranker stores match priority");
    return EXIT_SUCCESS;
}
