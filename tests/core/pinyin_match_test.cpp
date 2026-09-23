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

    // Typo matching tests (gn <-> ng, mg -> ng, ina <-> ian, uei -> ui, etc.)
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("dign", "ding") == 4,
               "dign typo-matches ding");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("xiagn", "xiang") == 5,
               "xiagn typo-matches xiang");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("zhogn", "zhong") == 5,
               "zhogn typo-matches zhong");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("zhegn", "zheng") == 5,
               "zhegn typo-matches zheng");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("dimg", "ding") == 4,
               "dimg typo-matches ding (m/n adjacent)");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("tina", "tian") == 4,
               "tina typo-matches tian");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("guna", "guan") == 4,
               "guna typo-matches guan");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("shuei", "shui") == 5,
               "shuei typo-matches shui");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("jiou", "jiu") == 4,
               "jiou typo-matches jiu");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("luen", "lun") == 4,
               "luen typo-matches lun");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("lve", "lue") == 3,
               "lve typo-matches lue");

    assertTrue(PinyinMatchPolicy::isTypoPrefix("dig", "ding"),
               "dig is a typo prefix for ding");
    assertTrue(PinyinMatchPolicy::isTypoPrefix("shue", "shui"),
               "shue is a typo prefix for shui");

    assertTrue(PinyinMatchPolicy::isFullTypoMatch("yuedign", "yue'ding"),
               "yuedign is a full typo match for yue'ding");
    assertTrue(PinyinMatchPolicy::isFullTypoMatch("xiagn", "xiang"),
               "xiagn is a full typo match for xiang");
    assertTrue(PinyinMatchPolicy::isFullTypoMatch("zhogn", "zhong"),
               "zhogn is a full typo match for zhong");
    assertTrue(!PinyinMatchPolicy::isFullTypoMatch("nihao", "ni'hao"),
               "exact match is not a typo match");

    assertTrue(PinyinMatchPolicy::priority("yuedign", "yue'ding") == 2,
               "full typo match receives priority 2");
    assertTrue(PinyinMatchPolicy::priority("xiagn", "xiang") == 2,
               "xiagn full typo match receives priority 2");
    assertTrue(PinyinMatchPolicy::priority("zhogn", "zhong") == 2,
               "zhogn full typo match receives priority 2");

    // normalizeTypoInput: debounce, omission, transposition, adjacent keys
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("zhoongguo") == "zhongguo",
               "zhoongguo debounces to zhongguo");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("sheeng") == "sheng",
               "sheeng debounces to sheng");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("good") == "good",
               "English word good retains double vowels");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("garag") == "garag",
               "English prefix garag is protected from typo corruption");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("zhogguo") == "zhongguo",
               "zhogguo omission recovery to zhongguo");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("xuesheg") == "xuesheng",
               "xuesheg omission recovery to xuesheng");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("beijig") == "beijing",
               "beijig omission recovery to beijing");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("pengyo") == "pengyou",
               "pengyo omission recovery to pengyou");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("chifna") == "chifan",
               "chifna transposition to chifan");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("yop") == "you",
               "yop adjacent key slip to you");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("xiab") == "xian",
               "xiab adjacent key slip to xian");
    assertTrue(PinyinMatchPolicy::normalizeTypoInput("bucup") == "bucuo",
               "bucup adjacent key slip to bucuo");

    // matchTypoSyllable with omission and adjacent
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("zhog", "zhong") == 4,
               "zhog typo matches zhong");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("yo", "you") == 2,
               "yo typo matches you");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("yop", "you") == 3,
               "yop typo matches you");
    assertTrue(PinyinMatchPolicy::matchTypoSyllable("fna", "fan") == 3,
               "fna typo matches fan");

    // isFullTypoMatch with compound words
    assertTrue(PinyinMatchPolicy::isFullTypoMatch("zhogguo", "zhong'guo"),
               "zhogguo is a full typo match for zhong'guo");
    assertTrue(PinyinMatchPolicy::isFullTypoMatch("pengyo", "peng'you"),
               "pengyo is a full typo match for peng'you");
    assertTrue(PinyinMatchPolicy::isFullTypoMatch("beijig", "bei'jing"),
               "beijig is a full typo match for bei'jing");
    assertTrue(!PinyinMatchPolicy::isFullTypoMatch("garag", "ga'rang"),
               "English prefix garag is not considered a typo for ga'rang");

    return EXIT_SUCCESS;
}
