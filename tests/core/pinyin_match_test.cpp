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
    assertTrue(PinyinMatchPolicy::priority("zongguo", "zong'guo") == 2,
               "exact pinyin keeps the highest priority");
    assertTrue(PinyinMatchPolicy::priority("zongguo", "zhong'guo") == 0,
               "fuzzy pinyin does not receive exact-match priority");
    assertTrue(PinyinMatchPolicy::trustedShortAbbreviationMatch(
                   "wsm", "wei'shen'me", "为什么"),
               "common phrase with an exact initial key is trusted");
    assertTrue(!PinyinMatchPolicy::trustedShortAbbreviationMatch(
                   "who", "wo'hen'hao", "我很好"),
               "ordinary English-shaped initials are not broadly trusted");
    assertTrue(PinyinMatchPolicy::trustedShortAbbreviationMatch(
                   "bj", "bei'jing", "北京"),
               "high-frequency initialism bj for 北京 is trusted");
    assertTrue(PinyinMatchPolicy::trustedShortAbbreviationMatch(
                   "dl", "deng'lu", "登录"),
               "high-frequency initialism dl for 登录 is trusted");
    assertTrue(PinyinMatchPolicy::initialsMatch("yqch", "yi'qi'chi"),
               "compound initial ch matches syllables");
    assertTrue(PinyinMatchPolicy::initialsMatch("bj", "bei'jing"),
               "initialsMatch handles regular syllables");

    std::vector<CandidateScore> candidates{
        {0, "你", "ni", 0.0F}, {1, "你好", "ni'hao", 0.0F}};
    const auto order = CandidateRanker::rank("nihao", candidates);
    assertTrue(order.size() == 2 && order[0] == 1,
               "exact candidate outranks a prefix candidate");
    assertTrue(candidates[1].match_priority == 2,
               "ranker stores match priority");

    std::vector<CandidateScore> fuzzyCandidates{
        {0, "中国", "zhong'guo", -100.0F},
        {1, "总过", "zong'guo", 100.0F},
    };
    const auto fuzzyOrder = CandidateRanker::rank("zongguo", fuzzyCandidates);
    assertTrue(fuzzyOrder.size() == 2 && fuzzyOrder[0] == 1,
               "exact pinyin outranks a stronger fuzzy decoder candidate");
    return EXIT_SUCCESS;
}
