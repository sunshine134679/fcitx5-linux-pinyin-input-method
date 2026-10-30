#include "modernime/core/candidate_ranker.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/recent_choice.h"
#if defined(MODERNIME_HAS_FCITX5)
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/ui/candidate_pagination.h"
#endif

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

namespace {
using namespace modernime;
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
struct Host final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed = text; }
};
class CandidateRankingRegression : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "modernime-ranking-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "empty-user.dict").string();
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root, ec); }
    static std::size_t rank(const core::CandidatePage &page, std::string_view text) {
        for (std::size_t i = 0; i < page.items.size(); ++i)
            if (page.items[i].text == text) return i;
        return page.items.size();
    }
    static void type(fcitx5::ModernIMEController &c, std::string_view input) {
        c.setContext("", "");
        for (char key : input) ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, key}));
        ASSERT_EQ(c.page().rawInput, input);
    }
    static void feedKeys(fcitx5::ModernIMEController &c, std::string_view input) {
        for (char key : input) ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, key}));
        ASSERT_EQ(c.page().rawInput, input);
    }
#if defined(MODERNIME_HAS_FCITX5)
    static double textWidth(std::string_view text, double font) {
        double result = 0;
        for (unsigned char ch : text) {
            if ((ch & 0xc0) != 0x80) result += (ch < 128 ? 0.55 : 1.0) * font;
        }
        return result;
    }
    static std::pair<std::size_t, std::size_t> physicalPosition(
        const core::CandidatePage &page, std::size_t global, double font = 20, double panel = 610) {
        auto metrics = ui::CandidateBarMetrics::reference(font, 9);
        metrics.panelWidth = panel;
        const auto boundaries = ui::CandidatePagination::partition(page.items, metrics,
            [font](std::string_view text) { return textWidth(text, font); });
        for (std::size_t n = 0; n < boundaries.size(); ++n) {
            const auto b = boundaries[n];
            if (global >= b.begin && global < b.end) return {n, global - b.begin};
        }
        throw std::runtime_error("candidate lost during pagination");
    }
#endif
    std::filesystem::path root;
    pinyin::PinyinDataPaths paths;
};

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_Shi) {
    pinyin::PinyinCandidateProvider provider(paths);
    ASSERT_TRUE(provider.append("shi"));
    ASSERT_FALSE(provider.page().items.empty());
    EXPECT_EQ(provider.page().items.front().text, "是");
    const auto shiRank = rank(provider.page(), "试");
    EXPECT_LT(shiRank, 5u);
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_Jian) {
    pinyin::PinyinCandidateProvider provider(paths);
    ASSERT_TRUE(provider.append("jian"));
    ASSERT_FALSE(provider.page().items.empty());
    EXPECT_EQ(provider.page().items.front().text, "见");
    const auto jianRank = rank(provider.page(), "键");
    EXPECT_LT(jianRank, 5u);
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_Nihaoshijie) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "nihaoshijie");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "你好世界");
    const auto niRank = rank(c.page(), "你");
    EXPECT_LT(niRank, 5u);
    EXPECT_EQ(c.page().items[niRank].consumedInputBytes, 2u);
    ASSERT_TRUE(c.select(niRank));
    EXPECT_EQ(host.committed, "你");
    EXPECT_EQ(c.page().rawInput, "haoshijie");
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_GeneralizedPrefixes_3_5_6_7_Syllables) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. Three syllables: nihaoma -> "你" in top 5, consumes 2 bytes
    {
        type(c, "nihaoma");
        ASSERT_FALSE(c.page().items.empty());
        EXPECT_EQ(c.page().items.front().text, "你好吗");
        const auto niRank = rank(c.page(), "你");
        EXPECT_LT(niRank, 5u) << "'你' should be in top 5 for 3-syllable nihaoma";
        EXPECT_EQ(c.page().items[niRank].consumedInputBytes, 2u);
        ASSERT_TRUE(c.select(niRank));
        EXPECT_EQ(host.committed, "你");
        EXPECT_EQ(c.page().rawInput, "haoma");
        c.reset();
    }

    // 2. Five syllables: woshizhongguoren -> "我" in top 5, consumes 2 bytes
    {
        type(c, "woshizhongguoren");
        ASSERT_FALSE(c.page().items.empty());
        EXPECT_EQ(c.page().items.front().text, "我是中国人");
        const auto woRank = rank(c.page(), "我");
        EXPECT_LT(woRank, 5u) << "'我' should be in top 5 for 5-syllable woshizhongguoren";
        EXPECT_EQ(c.page().items[woRank].consumedInputBytes, 2u);
        ASSERT_TRUE(c.select(woRank));
        EXPECT_EQ(host.committed, "我");
        EXPECT_EQ(c.page().rawInput, "shizhongguoren");
        c.reset();
    }

    // 3. Six syllables: nihaoshijiepengyou -> "你" in top 5, consumes 2 bytes
    {
        type(c, "nihaoshijiepengyou");
        ASSERT_FALSE(c.page().items.empty());
        EXPECT_EQ(c.page().items.front().text, "你好世界朋友");
        const auto niRank = rank(c.page(), "你");
        EXPECT_LT(niRank, 5u) << "'你' should be in top 5 for 6-syllable nihaoshijiepengyou";
        EXPECT_EQ(c.page().items[niRank].consumedInputBytes, 2u);
        ASSERT_TRUE(c.select(niRank));
        EXPECT_EQ(host.committed, "你");
        EXPECT_EQ(c.page().rawInput, "haoshijiepengyou");
        c.reset();
    }

    // 4. Seven syllables: qingbangwodakaiwenjian -> "请" in top 5, consumes 4 bytes
    {
        type(c, "qingbangwodakaiwenjian");
        ASSERT_FALSE(c.page().items.empty());
        EXPECT_EQ(c.page().items.front().text, "请帮我打开文件");
        const auto qingRank = rank(c.page(), "请");
        EXPECT_LT(qingRank, 5u) << "'请' should be in top 5 for 7-syllable qingbangwodakaiwenjian";
        EXPECT_EQ(c.page().items[qingRank].consumedInputBytes, 4u);
        ASSERT_TRUE(c.select(qingRank));
        EXPECT_EQ(host.committed, "请");
        EXPECT_EQ(c.page().rawInput, "bangwodakaiwenjian");
        c.reset();
    }
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_GeneralizedPrefixes_30InputsComprehensive) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    struct Case {
        std::string input;
        std::string expectedFirstChar;
        std::size_t expectedFirstBytes;
        bool expectTop5SingleChar;
        std::string expectedFullWord;
    };
    const std::vector<Case> testCases = {
        // 2 syllables (retain native behavior without forcing long sentence slotting)
        {"nihao", "你", 2, false, "你好"},
        {"xiexie", "谢", 3, false, "谢谢"},
        {"zhongguo", "中", 5, false, "中国"},
        // 3 syllables
        {"nihaoma", "你", 2, true, "你好吗"},
        {"duibuqi", "对", 3, true, "对不起"},
        {"meiguanxi", "没", 3, true, "没关系"},
        {"chifanle", "吃", 3, true, "吃饭了"},
        {"zaishangmian", "在", 3, true, "在上面"},
        // 4 syllables
        {"nihaoshijie", "你", 2, true, "你好世界"},
        {"gongxifacai", "恭", 4, true, "恭喜发财"},
        {"wanwuhulian", "万", 3, true, "万物互联"},
        {"xinniankuaile", "新", 3, true, "新年快乐"},
        {"chunjiekuaile", "春", 4, true, "春节快乐"},
        // 5 syllables
        {"woshizhongguoren", "我", 2, true, "我是中国人"},
        {"jintiantianqihao", "今", 3, true, "今天天气好"},
        {"mingtianwoyoukong", "明", 4, true, "明天我有空"},
        {"zhelihenpiaoliang", "这", 3, true, "这里很漂亮"},
        {"tashiyigehaoren", "他", 2, true, "他是一个好人"},
        // 6 syllables
        {"nihaoshijiepengyou", "你", 2, true, "你好世界朋友"},
        {"womenshizhongguoren", "我", 2, true, "我们是中国人"},
        {"womenyaonulixuexi", "我", 2, true, "我们要努力学习"},
        {"dajiadouhenkaixin", "大", 2, true, "大家都很开心"},
        {"xueshengmenzaidushu", "学", 3, true, "学生们在读书"},
        // 7 syllables
        {"qingbangwodakaiwenjian", "请", 4, true, "请帮我打开文件"},
        {"woxiangyaohebeikafei", "我", 2, true, ""},
        {"qingwenxianzaijidianle", "请", 4, true, "请问现在几点了"},
        {"nengbunenggeiwoyidian", "能", 4, true, "能不能给我一点"},
        // 8 syllables
        {"womenxuyaogengduodeshijian", "我", 2, true, "我们需要更多的时间"},
        {"ruguoniyourenheyiwenqing", "如", 2, true, "如果你有任何疑问请"},
        {"xiwangdajiadounenggouchenggong", "希", 2, true, "希望大家都能够成功"}
    };
    ASSERT_EQ(testCases.size(), 30u);

    for (const auto &tc : testCases) {
        c.reset();
        type(c, tc.input);
        ASSERT_FALSE(c.page().items.empty()) << "Empty page for " << tc.input;
        if (!tc.expectedFullWord.empty()) {
            EXPECT_EQ(c.page().items.front().text, tc.expectedFullWord)
                << "Best full conversion mismatch for " << tc.input;
        }
        if (tc.expectTop5SingleChar) {
            const auto r = rank(c.page(), tc.expectedFirstChar);
            EXPECT_LT(r, 5u) << "Failed top 5 check for input '" << tc.input
                             << "', target '" << tc.expectedFirstChar << "'";
            if (r < c.page().items.size()) {
                EXPECT_EQ(c.page().items[r].consumedInputBytes, tc.expectedFirstBytes)
                    << "Byte consumption mismatch for input '" << tc.input << "'";
            }
        }
    }
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_Generalizability32CoreAnd20Holdout) {
    pinyin::PinyinCandidateProvider provider(paths);
    const std::vector<std::string> core32 = {
        "ba", "bo", "de", "di", "du", "ge", "gu", "he", "hu", "ji",
        "jia", "jian", "ke", "ku", "le", "lu", "ma", "mo", "mu", "na",
        "ni", "pa", "po", "qi", "qia", "qian", "qu", "shi", "te", "ti",
        "tu", "xi"
    };
    for (const auto &syl : core32) {
        provider.reset();
        ASSERT_TRUE(provider.append(syl));
        ASSERT_GE(provider.page().items.size(), 5u);
        if (syl == "shi") {
            EXPECT_EQ(provider.page().items.front().text, "是");
            EXPECT_LT(rank(provider.page(), "试"), 5u);
        } else if (syl == "jian") {
            EXPECT_EQ(provider.page().items.front().text, "见");
            EXPECT_LT(rank(provider.page(), "键"), 5u);
        } else if (syl == "de") {
            EXPECT_EQ(provider.page().items[0].text, "的");
            EXPECT_EQ(provider.page().items[1].text, "得");
        }
    }

    const std::vector<std::string> holdout20 = {
        "shishi", "jianpan", "nihao", "gongzuo", "xuexi",
        "kaishi", "women", "tamen", "zhege", "yige",
        "zhongguo", "fazhan", "shehui", "jingji", "wenhua",
        "keji", "shijie", "guojia", "renmin", "shenghuo",
        "gongsi", "fuwu"
    };
    for (const auto &pair : holdout20) {
        provider.reset();
        ASSERT_TRUE(provider.append(pair));
        ASSERT_FALSE(provider.page().items.empty());
    }
}

TEST_F(CandidateRankingRegression, RepeatedPartialChoiceReachesSecondSeatWithoutConsumingSuffix) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    for (int n = 0; n < 5; ++n) {
        type(c, "nihaoshijie");
        const auto i = rank(c.page(), "你");
        ASSERT_LT(i, c.page().items.size());
        ASSERT_TRUE(c.select(i));
        EXPECT_EQ(host.committed, "你");
        EXPECT_EQ(c.page().rawInput, "haoshijie");
        ASSERT_TRUE(c.handle({fcitx5::KeyKind::Escape}));
    }
    type(c, "nihaoshijie");
    EXPECT_LE(rank(c.page(), "你"), 1u);
    EXPECT_EQ(c.page().items.front().consumedInputBytes, 11u);
    ASSERT_TRUE(provider.flush());
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    const auto entry = snapshot->entry("你", "ni", "", "");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->frequency, 5);
    EXPECT_EQ(snapshot->entry("你", "nihaoshijie", "", ""), nullptr);
}

TEST_F(CandidateRankingRegression, DerivedPrefixUsesItsOwnPersistedLearning) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    for (int n = 0; n < 3; ++n) {
        type(c, "nihaoma");
        ASSERT_TRUE(c.select(rank(c.page(), "你好")));
        EXPECT_EQ(c.page().rawInput, "ma");
        ASSERT_TRUE(c.handle({fcitx5::KeyKind::Escape}));
    }
    ASSERT_TRUE(provider.flush());
    c.reset();
    type(c, "nihaoma");
    const auto i = rank(c.page(), "你好");
    ASSERT_LT(i, c.page().items.size());
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    EXPECT_GT(c.page().items[i].learningBoost, 1.8);
    EXPECT_NEAR(c.page().items[i].learningBoost,
                store.snapshot()->boostAt("你好", "ni'hao", nowMs()), 0.001);
    EXPECT_EQ(c.page().items[i].consumedInputBytes, 5u);
    EXPECT_EQ(c.page().items.front().consumedInputBytes, 7u);
}

TEST(CandidateCompositionRegression, DerivedPrefixDoesNotInheritParentsLearning) {
    core::CandidateItem full;
    full.text = "你好世界";
    full.fullPinyin = "ni'hao'shi'jie";
    full.consumedInputBytes = 11;
    full.learningBoost = 3.9;
    const auto mixed = pinyin::mixCandidateItems({full}, {}, &full, {2,5,8}, "nihaoshijie");
    for (const auto &item : mixed) {
        if (item.consumedInputBytes < 11) { EXPECT_DOUBLE_EQ(item.learningBoost, 0.0); }
    }
}

TEST(CandidateCompositionRegression, StrongPersonalPrefixKeepsFullSentenceQuota) {
    core::CandidateItem full1, full2, full3, partialItem;
    full1.text = "甲乙丙丁"; full1.fullPinyin = "jia'yi'bing'ding"; full1.consumedInputBytes = 13;
    full2.text = "甲乙丙钉"; full2.fullPinyin = "jia'yi'bing'ding"; full2.consumedInputBytes = 13;
    full3.text = "甲乙丙订"; full3.fullPinyin = "jia'yi'bing'ding"; full3.consumedInputBytes = 13;
    std::vector<core::CandidateItem> full{full1, full2, full3};

    partialItem.text = "甲"; partialItem.fullPinyin = "jia"; partialItem.source = core::CandidateSource::Learned;
    partialItem.consumedInputBytes = 3; partialItem.learningBoost = 3.9;
    std::vector<core::CandidateItem> partial{partialItem};

    const auto mixed = pinyin::mixCandidateItems(full, partial, &full.front(), {3,5,9}, "jiayibingding");
    ASSERT_GE(mixed.size(), 5u);
    EXPECT_EQ(mixed[0].text, "甲乙丙丁");
    EXPECT_EQ(mixed[1].text, "甲");
    std::size_t complete = 0;
    for (std::size_t i = 0; i < 5; ++i) complete += mixed[i].consumedInputBytes == 13;
    EXPECT_EQ(complete, 2u);
}

TEST(CandidatePriorRegression, SaturatedLearningCanOvercomeUnlearnedStaticPrior) {
    core::CandidateScore incumbent, target;
    incumbent.text="事实"; incumbent.full_pinyin="shi'shi";
    incumbent.dictionary_bonus=4; incumbent.is_lexical=true;
    target.text="实事"; target.full_pinyin="shi'shi";
    target.source_index=100; target.is_lexical=true; target.learning_boost=3.9;
    std::vector<core::CandidateScore> candidates{incumbent,target};
    const auto order=core::CandidateRanker::rank("shishi",candidates);
    EXPECT_EQ(candidates[order.front()].text,"实事");
}

TEST(CandidatePriorRegression, LearnedSingleCharCannotHijackAbbreviationInput) {
    // 2026-10-25 发布门禁回归（Ubuntu 22.04 实机复现）：简拼 nh 的原生候选
    // 中含单字「你」(ni)，学习增益 ≥1.8 且音节数（1）与输入段数（1）相同，
    // 被 F1 模糊音组间晋升误抬到 trusted 简拼「你好」之上。模糊音晋升只应
    // 作用于全拼输入（每个音节均为标准音节），简拼/混拼不得借用该通道。
    core::CandidateScore hijacker, trusted;
    hijacker.text = "你";
    hijacker.full_pinyin = "ni";
    hijacker.learning_boost = 3.0;
    trusted.text = "你好";
    trusted.full_pinyin = "ni'hao";
    std::vector<core::CandidateScore> candidates{hijacker, trusted};
    const auto order = core::CandidateRanker::rank("nh", candidates);
    EXPECT_EQ(candidates[order.front()].text, "你好");
}

TEST_F(CandidateRankingRegression, NativeCandidatePreservesNegativeLearningEvidence) {
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.recordBatch({
            {core::LearningEvent::Kind::Selection, "试", "shi", {}, {}, nowMs()},
            {core::LearningEvent::Kind::NegativeFeedback, "试", "shi", {}, {}, nowMs()},
            {core::LearningEvent::Kind::NegativeFeedback, "试", "shi", {}, {}, nowMs()},
            {core::LearningEvent::Kind::NegativeFeedback, "试", "shi", {}, {}, nowMs()}}));
    }
    pinyin::PinyinCandidateProvider provider(paths);
    ASSERT_TRUE(provider.append("shi"));
    const auto &candidate = provider.page().items.at(rank(provider.page(), "试"));
    EXPECT_LT(candidate.learningBoost, 0.0);
    // Equal static evidence must prefer the neutral item over the negatively
    // learned one, even when the negative item's identity wins a lexical tie.
    std::vector<core::CandidateScore> evidence{
        {.text="乙", .full_pinyin="shi", .learning_boost=candidate.learningBoost},
        {.text="甲", .full_pinyin="shi"}};
    const auto order = core::CandidateRanker::rank("shi", evidence);
    EXPECT_EQ(evidence[order.front()].text, "甲");
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    EXPECT_NEAR(candidate.learningBoost, store.snapshot()->boostAt("试", "shi", nowMs()), 0.001);
}

TEST_F(CandidateRankingRegression, LearningDoesNotRewriteNativeCandidateReference) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "shi");
    const auto original = c.page().items.at(rank(c.page(), "试")).sourceIndex;
    for (int n = 0; n < 3; ++n) {
        ASSERT_TRUE(c.select(rank(c.page(), "试")));
        type(c, "shi");
    }
    EXPECT_EQ(c.page().items.at(rank(c.page(), "试")).sourceIndex, original);
}

TEST(CandidatePriorRegression, EqualEvidenceHasDeterministicOrderAcrossRecallEnumeration) {
    std::vector<core::CandidateScore> forward{
        {.source_index=0, .text="甲", .full_pinyin="jia"},
        {.source_index=0, .text="乙", .full_pinyin="jia"}};
    auto reverse = forward;
    std::reverse(reverse.begin(), reverse.end());
    const auto a = core::CandidateRanker::rank("jia", forward);
    const auto b = core::CandidateRanker::rank("jia", reverse);
    EXPECT_EQ(forward[a.front()].text, reverse[b.front()].text);
}

TEST_F(CandidateRankingRegression, TenChoicesPromoteDeepNativeWordWithoutSessionIntent) {
    pinyin::PinyinCandidateProvider provider(paths);
    for (int n = 0; n < 10; ++n) {
        provider.reset();
        provider.resetCommitHistory();
        provider.setContext("", "");
        for (char key : std::string("shishi")) ASSERT_TRUE(provider.append(std::string(1,key)));
        const auto i = rank(provider.page(), "实是");
        ASSERT_LT(i, provider.page().items.size());
        ASSERT_TRUE(provider.select(i));
    }
    provider.reset();
    provider.resetCommitHistory();
    ASSERT_TRUE(provider.append("shishi"));
    EXPECT_LT(rank(provider.page(), "实是"), 5u);
}

class StrongCompetitionRegression : public CandidateRankingRegression,
    public ::testing::WithParamInterface<std::tuple<std::string,std::string,std::string>> {};

TEST_P(StrongCompetitionRegression, TenRecentChoicesSwitchPreferenceWithoutSingleClickHijacking) {
    const auto &[input, target, incumbent] = GetParam();
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        const std::vector<core::LearningEvent> seed(200,
            {core::LearningEvent::Kind::Selection, incumbent, input, {}, {}, nowMs()});
        ASSERT_TRUE(store.recordBatch(seed));
    }
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, input);
    for (int n = 1; n <= 10; ++n) {
        const auto i = rank(c.page(), target);
        ASSERT_LT(i, c.page().items.size());
        ASSERT_TRUE(c.select(i));
        type(c, input);
        if (n == 1) { EXPECT_EQ(c.page().items.front().text, incumbent); }
    }
    EXPECT_EQ(c.page().items.front().text, target);
    ASSERT_TRUE(provider.flush());
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    const auto persisted = snapshot->entry(target, input, "", "");
    ASSERT_NE(persisted, nullptr);
    EXPECT_EQ(persisted->frequency, 10);
}

TEST_P(StrongCompetitionRegression, FiftySelectionsRetainPreferenceCrossSessionInFreshProcess) {
    const auto &[input, target, incumbent] = GetParam();
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        const std::vector<core::LearningEvent> seed(200,
            {core::LearningEvent::Kind::Selection, incumbent, input, {}, {}, nowMs()});
        ASSERT_TRUE(store.recordBatch(seed));
    }
    {
        pinyin::PinyinCandidateProvider provider(paths);
        Host host;
        fcitx5::ModernIMEController c(host, &provider);
        type(c, input);
        // 1. 单次偶发选择 target：绝对不能篡夺首选位置
        const auto i1 = rank(c.page(), target);
        ASSERT_LT(i1, c.page().items.size());
        ASSERT_TRUE(c.select(i1));
        type(c, input);
        EXPECT_EQ(c.page().items.front().text, incumbent);

        // 2. 连续选择达到 10 次：在会话内即时翻转为首位
        for (int n = 2; n <= 10; ++n) {
            const auto i = rank(c.page(), target);
            ASSERT_LT(i, c.page().items.size());
            ASSERT_TRUE(c.select(i));
            type(c, input);
        }
        EXPECT_EQ(c.page().items.front().text, target);

        // 3. 累计选择达到 50 次
        for (int n = 11; n <= 50; ++n) {
            const auto i = rank(c.page(), target);
            ASSERT_LT(i, c.page().items.size());
            ASSERT_TRUE(c.select(i));
            type(c, input);
        }
        EXPECT_EQ(c.page().items.front().text, target);
        ASSERT_TRUE(provider.flush());
    }

    // 4. 在全新的子进程中跨会话冷启动，读取同一数据库，target 必须稳居首位
    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        pinyin::PinyinCandidateProvider childProvider(paths);
        if (!childProvider.append(input)) {
            _exit(1);
        }
        if (childProvider.page().items.empty() || childProvider.page().items.front().text != target) {
            std::cerr << "Child process check failed: expected " << target
                      << " at rank 0, got "
                      << (childProvider.page().items.empty() ? "<empty>" : childProvider.page().items.front().text)
                      << " for input " << input << std::endl;
            _exit(2);
        }
        _exit(0);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

INSTANTIATE_TEST_SUITE_P(UserExamples, StrongCompetitionRegression, ::testing::Values(
    std::make_tuple("shi","试","是"), std::make_tuple("shi","是","试"),
    std::make_tuple("jian","键","见"), std::make_tuple("jian","见","键")));

TEST_F(CandidateRankingRegression, DisablingLearningDoesNotRestorePersistentlySuppressedCandidates) {
    {
        pinyin::PinyinCandidateProvider provider(paths);
        ASSERT_TRUE(provider.append("nihao"));
        ASSERT_TRUE(provider.remove(rank(provider.page(), "你好")));
        provider.reset();
        ASSERT_TRUE(provider.append("gara"));
        ASSERT_TRUE(provider.remove(rank(provider.page(), "garage")));
        ASSERT_TRUE(provider.flush());
    }
    pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    pinyin::PinyinCandidateProvider restarted(paths, options);
    ASSERT_TRUE(restarted.append("nihao"));
    EXPECT_EQ(rank(restarted.page(), "你好"), restarted.page().items.size());
    restarted.reset();
    ASSERT_TRUE(restarted.append("gara"));
    EXPECT_EQ(rank(restarted.page(), "garage"), restarted.page().items.size());
    restarted.reset();
    ASSERT_TRUE(restarted.append("nihaoma"));
    EXPECT_EQ(rank(restarted.page(), "你好"), restarted.page().items.size());
    auto shared = pinyin::PinyinCandidateProvider::createSharedResources(paths, options);
    pinyin::PinyinCandidateProvider other(shared, options);
    ASSERT_TRUE(other.append("nihao"));
    EXPECT_EQ(rank(other.page(), "你好"), other.page().items.size());
    core::LearningStore settings(paths.learningStore);
    ASSERT_TRUE(settings.open());
    ASSERT_TRUE(settings.clear());
    EXPECT_TRUE(pinyin::PinyinCandidateProvider::refreshLearningStore(shared));
    other.reset();
    ASSERT_TRUE(other.append("nihao"));
    EXPECT_LT(rank(other.page(), "你好"), other.page().items.size());
}

TEST_F(CandidateRankingRegression, RecentIntentIsLocalAndResetDoesNotErasePersistentLearning) {
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.recordBatch(std::vector<core::LearningEvent>(200,
            {core::LearningEvent::Kind::Selection, "是", "shi", {}, {}, nowMs()})));
    }
    auto shared = pinyin::PinyinCandidateProvider::createSharedResources(paths, {});
    pinyin::PinyinCandidateProvider first(shared), second(shared);
    Host host;
    fcitx5::ModernIMEController c(host, &first);
    type(c, "shi");
    for (int n = 0; n < 10; ++n) {
        ASSERT_TRUE(c.select(rank(c.page(), "试")));
        type(c, "shi");
    }
    ASSERT_EQ(c.page().items.front().text, "试");
    ASSERT_TRUE(second.append("shi"));
    EXPECT_EQ(second.page().items.front().text, "是");
    c.reset();
    type(c, "shi");
    EXPECT_EQ(c.page().items.front().text, "是");
    ASSERT_TRUE(first.flush());
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    ASSERT_NE(snapshot->entry("试", "shi", "", ""), nullptr);
    EXPECT_EQ(snapshot->entry("试", "shi", "", "")->frequency, 10);
}

TEST_F(CandidateRankingRegression, SensitiveAndDisabledChoicesDoNotPrimeRecentIntent) {
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.recordBatch(std::vector<core::LearningEvent>(200,
            {core::LearningEvent::Kind::Selection, "是", "shi", {}, {}, nowMs()})));
    }
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    c.setSensitive(true);
    for (int n = 0; n < 10; ++n) {
        type(c, "shi");
        ASSERT_TRUE(c.select(rank(c.page(), "试")));
    }
    c.setSensitive(false);
    provider.setLearningEnabled(false);
    for (int n = 0; n < 10; ++n) {
        type(c, "shi");
        ASSERT_TRUE(c.select(rank(c.page(), "试")));
    }
    provider.setLearningEnabled(true);
    for (int n = 0; n < 3; ++n) {
        type(c, "shi");
        ASSERT_TRUE(c.select(rank(c.page(), "试")));
    }
    type(c, "shi");
    EXPECT_EQ(c.page().items.front().text, "是");
    ASSERT_TRUE(provider.flush());
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    ASSERT_NE(snapshot->entry("试", "shi", "", ""), nullptr);
    EXPECT_EQ(snapshot->entry("试", "shi", "", "")->frequency, 3);
}

TEST(RecentChoiceRegression, RequiresRepeatedMajorityAndForgetsOverwrittenWindow) {
    pinyin::RecentChoices choices;
    choices.record("shi", "试", "shi", false);
    choices.record("shi", "试", "shi", false);
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "试", "shi", false), 0.0);
    choices.record("shi", "试", "shi", false);
    EXPECT_GT(choices.bonus("shi", "试", "shi", false), 0.0);
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "试", "shi", true), 0.0);
    for (int n = 0; n < 3; ++n) choices.record("shi", "是", "shi", false);
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "试", "shi", false), 0.0);
    EXPECT_GT(choices.bonus("shi", "是", "shi", false), 0.0);
    for (int n = 0; n < 8; ++n) choices.record("shi", "是", "shi", false);
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "试", "shi", false), 0.0);
    EXPECT_GT(choices.bonus("shi", "是", "shi", false), 0.0);
    choices.clear();
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "是", "shi", false), 0.0);
}

TEST_F(CandidateRankingRegression, EnglishLearningReachability_SaturatedPreferenceTakeoverAndReverseSwitch) {
    const auto now = nowMs();
    // 1. Snapshot-level verification: 200 seed compute, 1 click computer -> compute #1
    {
        core::LearningSnapshot s1({
            {"compute", "compute", {}, {}, 200, now - 1000, 0, false},
            {"computer", "computer", {}, {}, 1, now, 0, false}
        });
        auto words1 = s1.recallEnglishWords("compu", 3, now);
        ASSERT_GE(words1.size(), 2u);
        EXPECT_EQ(words1[0].text, "compute");
        EXPECT_EQ(words1[1].text, "computer");
    }
    // 2. Snapshot-level verification: 200 seed compute, 50 clicks computer -> computer #1!
    {
        core::LearningSnapshot s50({
            {"compute", "compute", {}, {}, 200, now - 1000, 0, false},
            {"computer", "computer", {}, {}, 50, now, 0, false}
        });
        auto words50 = s50.recallEnglishWords("compu", 3, now);
        ASSERT_GE(words50.size(), 2u);
        EXPECT_EQ(words50[0].text, "computer");
        EXPECT_EQ(words50[1].text, "compute");
    }
    // 3. Snapshot-level verification: 250 and 5000 frequency saturation
    {
        core::LearningSnapshot s250({
            {"compute", "compute", {}, {}, 200, now - 1000, 0, false},
            {"computer", "computer", {}, {}, 250, now, 0, false}
        });
        auto words250 = s250.recallEnglishWords("compu", 3, now);
        ASSERT_GE(words250.size(), 2u);
        EXPECT_EQ(words250[0].text, "computer");

        core::LearningSnapshot s5000({
            {"compute", "compute", {}, {}, 200, now - 1000, 0, false},
            {"computer", "computer", {}, {}, 5000, now, 0, false}
        });
        auto words5000 = s5000.recallEnglishWords("compu", 3, now);
        ASSERT_GE(words5000.size(), 2u);
        EXPECT_EQ(words5000[0].text, "computer");
    }
    // 4. Reverse switch: 250 seed computer, 50 clicks compute -> compute #1!
    {
        core::LearningSnapshot sRev({
            {"computer", "computer", {}, {}, 250, now - 1000, 0, false},
            {"compute", "compute", {}, {}, 50, now, 0, false}
        });
        auto wordsRev = sRev.recallEnglishWords("compu", 3, now);
        ASSERT_GE(wordsRev.size(), 2u);
        EXPECT_EQ(wordsRev[0].text, "compute");
        EXPECT_EQ(wordsRev[1].text, "computer");
    }
    // 5. Generalization over 10 different word pairs
    const std::vector<std::pair<std::string, std::string>> wordPairs = {
        {"develop", "developer"},
        {"program", "programmer"},
        {"connect", "connection"},
        {"system", "systematic"},
        {"manage", "management"},
        {"direct", "direction"},
        {"person", "personal"},
        {"differ", "difference"},
        {"inform", "information"},
        {"expect", "expectation"}
    };
    for (const auto &[shorter, longer] : wordPairs) {
        std::string prefix = shorter.substr(0, std::min<std::size_t>(5, shorter.size()));
        core::LearningSnapshot sPair({
            {shorter, shorter, {}, {}, 200, now - 1000, 0, false},
            {longer, longer, {}, {}, 50, now, 0, false}
        });
        auto recalled = sPair.recallEnglishWords(prefix, 3, now);
        ASSERT_GE(recalled.size(), 2u);
        EXPECT_EQ(recalled[0].text, longer) << "Failed for pair " << shorter << " -> " << longer;
    }
}

TEST_F(CandidateRankingRegression, EnglishLearningReachability_EndToEndControllerPersistenceAndReverse) {
    for (const bool useLmdb : {false, true}) {
        auto testRoot = root / (useLmdb ? "lmdb_case" : "sqlite_case");
        std::filesystem::create_directories(testRoot);
        pinyin::PinyinDataPaths currentPaths;
        currentPaths.learningStore = (testRoot / (useLmdb ? "learning.lmdb" : "learning.sqlite3")).string();
        currentPaths.userDictionary = paths.userDictionary;

        const auto seedTime = nowMs() - 60000;
        // 1. Seed 200 occurrences of "compute"
        {
            core::LearningStore store(currentPaths.learningStore);
            ASSERT_TRUE(store.open());
            std::vector<core::LearningEvent> seedEvents;
            for (int i = 0; i < 200; ++i) {
                seedEvents.push_back({core::LearningEvent::Kind::Selection, "compute", "compute", {}, {}, seedTime + i});
            }
            ASSERT_TRUE(store.recordBatch(seedEvents));
        }

        // 2. Open provider & controller, type "compu"
        {
            pinyin::PinyinCandidateProvider provider(currentPaths);
            Host host;
            fcitx5::ModernIMEController c(host, &provider);
            type(c, "compu");
            const auto computeRank = rank(c.page(), "compute");
            const auto computerRank = rank(c.page(), "computer");
            ASSERT_LT(computeRank, c.page().items.size());
            ASSERT_LT(computerRank, c.page().items.size());
            EXPECT_LT(computeRank, computerRank) << "compute should start ahead of computer";

            // 1 selection of computer: compute should still beat computer
            ASSERT_TRUE(c.select(computerRank));
            ASSERT_TRUE(provider.flush());

            type(c, "compu");
            const auto cRank1 = rank(c.page(), "compute");
            const auto crRank1 = rank(c.page(), "computer");
            EXPECT_LT(cRank1, crRank1) << "Accidental click on computer must not usurp compute as #1";
            c.reset();

            // Train computer up to 50 times
            for (int i = 1; i < 50; ++i) {
                type(c, "compu");
                const auto r = rank(c.page(), "computer");
                ASSERT_LT(r, c.page().items.size());
                ASSERT_TRUE(c.select(r));
            }
            ASSERT_TRUE(provider.flush());

            // Within session, computer must now beat compute
            type(c, "compu");
            EXPECT_LT(rank(c.page(), "computer"), rank(c.page(), "compute"));
        }

        // 3. New fresh instance reading the same store
        {
            pinyin::PinyinCandidateProvider freshProvider(currentPaths);
            Host freshHost;
            fcitx5::ModernIMEController freshC(freshHost, &freshProvider);
            type(freshC, "compu");
            EXPECT_LT(rank(freshC.page(), "computer"), rank(freshC.page(), "compute"))
                << "Must persist in fresh instance (LMDB=" << useLmdb << ")";
            freshC.reset();

            // 4. Reverse switch: select compute 50 times in fresh instance
            for (int i = 0; i < 50; ++i) {
                type(freshC, "compu");
                const auto r = rank(freshC.page(), "compute");
                ASSERT_LT(r, freshC.page().items.size());
                ASSERT_TRUE(freshC.select(r));
            }
            ASSERT_TRUE(freshProvider.flush());

            // Within session, compute should take over again
            type(freshC, "compu");
            EXPECT_LT(rank(freshC.page(), "compute"), rank(freshC.page(), "computer"));
        }

        // 5. Another fresh instance to confirm reverse switch persisted
        {
            pinyin::PinyinCandidateProvider freshProvider2(currentPaths);
            Host freshHost2;
            fcitx5::ModernIMEController freshC2(freshHost2, &freshProvider2);
            type(freshC2, "compu");
            EXPECT_LT(rank(freshC2.page(), "compute"), rank(freshC2.page(), "computer"))
                << "Reverse switch must persist (LMDB=" << useLmdb << ")";
        }
    }
}

TEST_F(CandidateRankingRegression, TiedTimestampPersistenceAndWriteOrderInvariance) {
    for (const bool useLmdb : {false, true}) {
        const auto now = nowMs();
        std::vector<std::string> orderA_cands;
        std::vector<std::string> orderB_cands;

        for (int pass = 0; pass < 2; ++pass) {
            auto testRoot = root / ((useLmdb ? "lmdb_tied_" : "sqlite_tied_") + std::to_string(pass));
            std::filesystem::create_directories(testRoot);
            pinyin::PinyinDataPaths currentPaths;
            currentPaths.learningStore = (testRoot / (useLmdb ? "learning.lmdb" : "learning.sqlite3")).string();
            currentPaths.userDictionary = paths.userDictionary;

            {
                core::LearningStore store(currentPaths.learningStore);
                ASSERT_TRUE(store.open());
                std::vector<core::LearningEvent> events;
                for (int i = 0; i < 200; ++i) {
                    events.push_back({core::LearningEvent::Kind::Selection, "是", "shi", {}, {}, now - 10000 + i});
                }
                if (pass == 0) {
                    for (int i = 0; i < 50; ++i) {
                        events.push_back({core::LearningEvent::Kind::Selection, "试", "shi", {}, {}, now});
                    }
                    for (int i = 0; i < 2; ++i) {
                        events.push_back({core::LearningEvent::Kind::Selection, "时", "shi", {}, {}, now});
                    }
                } else {
                    for (int i = 0; i < 2; ++i) {
                        events.push_back({core::LearningEvent::Kind::Selection, "时", "shi", {}, {}, now});
                    }
                    for (int i = 0; i < 50; ++i) {
                        events.push_back({core::LearningEvent::Kind::Selection, "试", "shi", {}, {}, now});
                    }
                }
                ASSERT_TRUE(store.recordBatch(events));
            }

            pinyin::PinyinCandidateProvider provider(currentPaths);
            Host host;
            fcitx5::ModernIMEController c(host, &provider);
            type(c, "shi");
            std::vector<std::string> top3;
            for (std::size_t i = 0; i < std::min<std::size_t>(3, c.page().items.size()); ++i) {
                top3.push_back(c.page().items[i].text);
            }
            if (pass == 0) {
                orderA_cands = top3;
            } else {
                orderB_cands = top3;
            }
        }
        ASSERT_FALSE(orderA_cands.empty());
        EXPECT_EQ(orderA_cands[0], "试");
        EXPECT_EQ(orderA_cands, orderB_cands) << "Write order caused candidate mismatch (LMDB=" << useLmdb << ")";
    }
}

TEST(CandidateRankerRegression, TiedLatestTimestampPermutationInvarianceAcrossAllSixPermutations) {
    using namespace modernime::core;
    CandidateScore oldCand, aCand, bCand;
    oldCand.text = "是";
    oldCand.full_pinyin = "shi";
    oldCand.source_index = 0;
    oldCand.is_lexical = true;
    oldCand.phrase_length = 1;
    oldCand.learning_boost = 3.9;
    oldCand.last_selected_ms = 1000000;

    aCand = oldCand;
    aCand.text = "试";
    aCand.source_index = 10;
    aCand.last_selected_ms = 1000001;

    bCand = oldCand;
    bCand.text = "时";
    bCand.source_index = 20;
    bCand.last_selected_ms = 1000001;
    bCand.learning_boost = 1.0;

    std::vector<CandidateScore> initial = {oldCand, aCand, bCand};
    std::vector<int> permIndices = {0, 1, 2};
    int permCount = 0;
    std::vector<std::string> canonicalOrder;
    std::vector<double> canonicalDormants;
    std::vector<double> canonicalScores;

    do {
        std::vector<CandidateScore> current = {
            initial[permIndices[0]],
            initial[permIndices[1]],
            initial[permIndices[2]]
        };
        auto order = CandidateRanker::rank("shi", current);
        ASSERT_EQ(order.size(), 3u);

        std::vector<std::string> orderedTexts;
        std::vector<double> orderedDormants;
        std::vector<double> orderedScores;
        for (auto idx : order) {
            orderedTexts.push_back(current[idx].text);
            orderedDormants.push_back(current[idx].dormant_factor);
            orderedScores.push_back(current[idx].final_score());
        }

        // The top candidate MUST be "试" in all permutations!
        EXPECT_EQ(orderedTexts[0], "试") << "Permutation #" << permCount << " failed";
        EXPECT_EQ(orderedTexts[1], "是") << "Permutation #" << permCount << " failed";
        EXPECT_EQ(orderedTexts[2], "时") << "Permutation #" << permCount << " failed";

        // Find "是" in current and verify dormant discount is applied
        for (const auto &c : current) {
            if (c.text == "是") {
                EXPECT_NEAR(c.dormant_factor, 0.70, 1e-4)
                    << "Permutation #" << permCount << " '是' dormant discount not applied!";
            } else {
                EXPECT_NEAR(c.dormant_factor, 1.0, 1e-4);
            }
        }

        if (permCount == 0) {
            canonicalOrder = orderedTexts;
            canonicalDormants = orderedDormants;
            canonicalScores = orderedScores;
        } else {
            EXPECT_EQ(orderedTexts, canonicalOrder)
                << "Order mismatch in permutation #" << permCount;
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(orderedDormants[i], canonicalDormants[i], 1e-6);
                EXPECT_NEAR(orderedScores[i], canonicalScores[i], 1e-6);
            }
        }
        ++permCount;
    } while (std::next_permutation(permIndices.begin(), permIndices.end()));

    EXPECT_EQ(permCount, 6);
}

TEST(CandidateRankerRegression, TiedTimestampEdgeConditions) {
    using namespace modernime::core;
    // 1. Older candidate with high boost vs newer candidate with low boost
    {
        CandidateScore oldHigh, newLow;
        oldHigh.text = "是";
        oldHigh.full_pinyin = "shi";
        oldHigh.last_selected_ms = 1000;
        oldHigh.learning_boost = 3.9;

        newLow.text = "试";
        newLow.full_pinyin = "shi";
        newLow.last_selected_ms = 2000;
        newLow.learning_boost = 1.0;

        std::vector<CandidateScore> cands = {oldHigh, newLow};
        CandidateRanker::rank("shi", cands);
        // Because newLow has boost 1.0 < 1.8, oldHigh must NOT receive dormant discount!
        for (const auto &c : cands) {
            EXPECT_NEAR(c.dormant_factor, 1.0, 1e-4);
        }
    }
    // 2. Zero timestamp and negative/non-finite values
    {
        CandidateScore zeroCand, nanCand, validCand;
        zeroCand.text = "零";
        zeroCand.full_pinyin = "ling";
        zeroCand.last_selected_ms = 0;
        zeroCand.learning_boost = 3.0;

        nanCand.text = "南";
        nanCand.full_pinyin = "ling";
        nanCand.last_selected_ms = 5000;
        nanCand.learning_boost = std::numeric_limits<double>::quiet_NaN();

        validCand.text = "另";
        validCand.full_pinyin = "ling";
        validCand.last_selected_ms = 6000;
        validCand.learning_boost = 2.5;

        std::vector<CandidateScore> cands = {zeroCand, nanCand, validCand};
        auto order = CandidateRanker::rank("ling", cands);
        ASSERT_EQ(order.size(), 3u);
        // Finite candidates must sort before NaN candidate
        EXPECT_TRUE(std::isfinite(cands[order[0]].final_score()));
        EXPECT_TRUE(std::isfinite(cands[order[1]].final_score()));
        EXPECT_FALSE(std::isfinite(cands[order[2]].final_score()));
        EXPECT_EQ(cands[order[2]].text, "南");
    }
    // 3. Independent priority groups
    {
        CandidateScore exactMatch, prefixMatch;
        exactMatch.text = "好";
        exactMatch.full_pinyin = "hao";
        exactMatch.match_priority = 2; // exact
        exactMatch.last_selected_ms = 10000;
        exactMatch.learning_boost = 3.9;

        prefixMatch.text = "豪迈";
        prefixMatch.full_pinyin = "hao'mai";
        prefixMatch.match_priority = 1; // prefix/abbreviation
        prefixMatch.last_selected_ms = 1000;
        prefixMatch.learning_boost = 3.9;

        std::vector<CandidateScore> cands = {exactMatch, prefixMatch};
        CandidateRanker::rank("hao", cands);
        // Prefix match is in a different priority group, so exactMatch's recent timestamp does not discount it
        EXPECT_DOUBLE_EQ(prefixMatch.dormant_factor, 1.0);
        EXPECT_DOUBLE_EQ(exactMatch.dormant_factor, 1.0);
    }
}

TEST(RecentChoiceRegression, BoundedInputHistoryEvictsLeastRecentlySelectedGroup) {
    pinyin::RecentChoices choices;
    for (int n = 0; n < 3; ++n) choices.record("shi", "试", "shi", false);
    for (int n = 0; n < 64; ++n) {
        const auto input = "fixture" + std::to_string(n);
        for (int k = 0; k < 3; ++k) choices.record(input, "甲", "jia", false);
    }
    EXPECT_DOUBLE_EQ(choices.bonus("shi", "试", "shi", false), 0.0);
    EXPECT_GT(choices.bonus("fixture63", "甲", "jia", false), 0.0);
}

// -----------------------------------------------------------------------------
// 17,430-Run Deep Boundary Tests
// -----------------------------------------------------------------------------

TEST_F(CandidateRankingRegression, EnglishDictionary_LongWordPrefixPrediction_Programming_Application_Typescript) {
    // Issue 1: High-frequency long words (programming, application, typescript) must appear in top 3 predictions
    {
        const auto progMatches = core::EnglishDictionary::predictWords("prog", 3);
        ASSERT_GE(progMatches.size(), 1u);
        EXPECT_EQ(progMatches.front(), "program");
        EXPECT_TRUE(std::find(progMatches.begin(), progMatches.end(), "programming") != progMatches.end())
            << "Unlearned static predictWords for 'prog' must contain 'programming' in top 3";
    }
    {
        const auto applMatches = core::EnglishDictionary::predictWords("appl", 3);
        ASSERT_GE(applMatches.size(), 1u);
        EXPECT_EQ(applMatches.front(), "apple");
        EXPECT_TRUE(std::find(applMatches.begin(), applMatches.end(), "application") != applMatches.end())
            << "Unlearned static predictWords for 'appl' must contain 'application' in top 3";
    }
    {
        const auto typeMatches = core::EnglishDictionary::predictWords("type", 3);
        ASSERT_GE(typeMatches.size(), 1u);
        EXPECT_EQ(typeMatches.front(), "type");
        EXPECT_TRUE(std::find(typeMatches.begin(), typeMatches.end(), "typescript") != typeMatches.end())
            << "Unlearned static predictWords for 'type' must contain 'typescript' in top 3";
    }

    // Verify in end-to-end controller with clean unlearned provider
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "prog");
    ASSERT_FALSE(c.page().items.empty());
    const auto progRank = rank(c.page(), "programming");
    EXPECT_LT(progRank, 5u) << "'programming' must appear in top 5 candidates for 'prog'";
}

TEST_F(CandidateRankingRegression, ColdStartWordDiscovery_7SyllableSingleCharOnPhysicalPageOne) {
    // Issue 2: For 7+ syllable input 'qingbangwodakaiwenjian', single char '请' must be on physical page 1
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "qingbangwodakaiwenjian");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "请帮我打开文件");

    const auto qingRank = rank(c.page(), "请");
    EXPECT_LT(qingRank, 5u) << "'请' should be in top 5 for 7-syllable qingbangwodakaiwenjian";

#if defined(MODERNIME_HAS_FCITX5)
    const auto [physicalPage, slot] = physicalPosition(c.page(), qingRank, 20.0, 610.0);
    EXPECT_EQ(physicalPage, 0u) << "Single character '请' must be on physical page 1 (0-indexed page 0)";
#endif

    EXPECT_EQ(c.page().items[qingRank].consumedInputBytes, 4u);
    ASSERT_TRUE(c.select(qingRank));
    EXPECT_EQ(host.committed, "请");
    EXPECT_EQ(c.page().rawInput, "bangwodakaiwenjian");
}

TEST_F(CandidateRankingRegression, MacroNumericInput_VModeCurrencyAmounts) {
    // Issue 3: In Controller, KeyKind::Character must accept digits and '.' in 'v' mode
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // Test v12345
    type(c, "v12345");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "壹万贰仟叁佰肆拾伍元整");
    ASSERT_TRUE(c.select(0));
    EXPECT_EQ(host.committed, "壹万贰仟叁佰肆拾伍元整");

    // Test v88.5
    c.reset();
    type(c, "v88.5");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "捌拾捌元伍角整");
    ASSERT_TRUE(c.select(0));
    EXPECT_EQ(host.committed, "捌拾捌元伍角整");
}

TEST_F(CandidateRankingRegression, MultiSyllableShortPinyin_PytTieBreakingStability) {
    // Issue 4: Deterministic tie-breaking for equal decoder score and match priority
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    std::set<std::string> variants;
    for (int n = 0; n < 30; ++n) {
        c.reset();
        type(c, "pyt");
        if (n % 3 == 1) {
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Backspace}));
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, 't'}));
        }
        if (n % 3 == 2) {
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::MoveCompositionLeft}));
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, 'n'}));
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Backspace}));
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::MoveCompositionRight}));
        }
        std::string top;
        for (std::size_t j = 0; j < std::min(std::size_t(10), c.page().items.size()); ++j) {
            top += (j ? "|" : "") + c.page().items[j].text;
        }
        variants.insert(top);
        auto expected = c.page().items.front().text;
        ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
        EXPECT_EQ(host.committed, expected);
    }
    EXPECT_EQ(variants.size(), 1u) << "Candidate ordering for 'pyt' must have exactly 1 variant across 30 repeats";
}

TEST_F(CandidateRankingRegression, EnglishDictionary_Prefix128Truncation_RecoversContextAndTranslate) {
    const auto contPred = core::EnglishDictionary::predictWords("cont", 3);
    EXPECT_FALSE(contPred.empty());
    bool hasContext = false;
    for (const auto &w : contPred) {
        if (w == "context") hasContext = true;
    }
    EXPECT_TRUE(hasContext) << "cont must predict 'context' in top 3 without 128-word alphabetical truncation";

    const auto transPred = core::EnglishDictionary::predictWords("trans", 3);
    EXPECT_FALSE(transPred.empty());
    bool hasTranslate = false;
    for (const auto &w : transPred) {
        if (w == "translate") hasTranslate = true;
    }
    EXPECT_TRUE(hasTranslate) << "trans must predict 'translate' in top 3 without 128-word alphabetical truncation";
}

TEST_F(CandidateRankingRegression, EnglishPrediction_RelaxedThreshold_3LetterAndExtension) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 3-letter prefix app -> apple / application
    c.reset();
    type(c, "app");
    bool hasApple = false;
    for (const auto &item : c.page().items) {
        if (item.text == "apple" || item.text == "application") hasApple = true;
    }
    EXPECT_TRUE(hasApple) << "Relaxed 3-letter threshold predicts 'apple' or 'application' for 'app'";

    // 3-letter prefix doc -> document
    c.reset();
    type(c, "doc");
    bool hasDocument = false;
    for (const auto &item : c.page().items) {
        if (item.text == "document") hasDocument = true;
    }
    EXPECT_TRUE(hasDocument) << "Relaxed 3-letter threshold predicts 'document' for 'doc'";

    // Word extension: compute -> computer
    c.reset();
    type(c, "compute");
    bool hasComputer = false;
    for (const auto &item : c.page().items) {
        if (item.text == "computer") hasComputer = true;
    }
    EXPECT_TRUE(hasComputer) << "Allow predicting 'computer' when input is valid word 'compute'";
}

TEST_F(CandidateRankingRegression, CrossArbitration_ShortPinyinPytPromotesPythonToTop3) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    c.reset();
    type(c, "pyt");
    ASSERT_GE(c.page().items.size(), 2u);

    // Chinese #1 must be preserved
    EXPECT_NE(c.page().items[0].text, "python") << "Chinese #1 must be preserved for 'pyt'";
    EXPECT_EQ(c.page().items[0].commitRole, core::CommitCandidateRole::Chinese);

    // Python should be promoted to slot 2 (index 1) or at least top 3
    bool pythonInTop3 = false;
    for (std::size_t i = 0; i < std::min(std::size_t(3), c.page().items.size()); ++i) {
        if (c.page().items[i].text == "python") {
            pythonInTop3 = true;
            break;
        }
    }
    EXPECT_TRUE(pythonInTop3) << "'python' should enter top 3 for consonant abbreviation 'pyt'";
}

TEST_F(CandidateRankingRegression, SharedResources_HotwordPathFallbackOnMissingFile) {
    auto fallbackPaths = paths;
    fallbackPaths.hotwordDictionary = (root / "nonexistent-hotwords.dict").string();
    pinyin::PinyinCandidateProvider provider(fallbackPaths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    c.reset();
    type(c, "yyds");
    bool hasYyds = false;
    for (const auto &item : c.page().items) {
        if (item.text == "永远滴神") {
            hasYyds = true;
            break;
        }
    }
    EXPECT_TRUE(hasYyds) << "Fallback to default hotword candidates preserves hotwords like '永远滴神'";
}

// -----------------------------------------------------------------------------
// AP01 / AP02 算法防污染回归测试集 (2026-10-27)
// 验证规范读音通用词频成熟后，单音节模糊音不跨组劫持留出精确读音候选
// -----------------------------------------------------------------------------

TEST_F(CandidateRankingRegression, AP01_AP02_NormalLearningDoesNotPolluteFuzzyPair_NeLe) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. 基线检查：冷启动输入 "ne"，首选必为 "呢" (精确匹配 prio=2)
    type(c, "ne");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "呢");
    EXPECT_EQ(c.page().items.front().fullPinyin, "ne");
    EXPECT_EQ(c.page().items.front().consumedInputBytes, 2u);
    c.reset();

    // 2. 正常上下文训练 "le -> 了" 20 次，建立成熟通用词频 (boost >= 1.8)
    for (int n = 0; n < 20; ++n) {
        c.setContext("完成", "", fcitx5::ContextAvailability::Valid);
        feedKeys(c, "le");
        const auto leRank = rank(c.page(), "了");
        ASSERT_LT(leRank, c.page().items.size());
        ASSERT_TRUE(c.select(leRank));
        EXPECT_EQ(host.committed, "了");
    }
    ASSERT_TRUE(provider.flush());

    // 验证学习库中已达到高词频，且拼音键严格记录为规范拼音 "le"
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.openReadOnly());
    const auto snapshot = store.snapshot();
    EXPECT_GE(snapshot->boostAt("了", "le", nowMs()), 1.8);
    EXPECT_EQ(snapshot->boostAt("了", "ne", nowMs()), 0.0);

    // 3. 留出输入 "ne" 测试（无上下文及特定上下文 "好的"）
    for (const auto &ctx : {"", "好的"}) {
        c.reset();
        c.setContext(ctx, "");
        type(c, "ne");
        ASSERT_FALSE(c.page().items.empty());

        // 核心断言：首选候选必须仍是精确单字 "呢"，绝不能被 "了" 劫持
        EXPECT_EQ(c.page().items.front().text, "呢")
            << "Exact match '呢' must retain #1 under context '" << ctx << "'";
        EXPECT_EQ(c.page().items.front().fullPinyin, "ne");
        EXPECT_EQ(core::PinyinMatchPolicy::priority(c.page().rawInput, c.page().items.front().fullPinyin), 2);

        // "了" 属于模糊音 (prio=0)，即使通用 boost >= 1.8 也不得晋升至首位
        const auto leRankAfter = rank(c.page(), "了");
        EXPECT_GT(leRankAfter, 0u) << "'了' must not occupy slot 0 for 'ne'";

#if defined(MODERNIME_HAS_FCITX5)
        // 物理候选栏布局检验：首选候选位于第 0 页第 0 槽
        const auto pos = physicalPosition(c.page(), 0);
        EXPECT_EQ(pos.first, 0u);
        EXPECT_EQ(pos.second, 0u);
#endif

        // 真实按键提交检验：空格键或选择 0 槽提交 "呢"
        ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
        EXPECT_EQ(host.committed, "呢");
        EXPECT_TRUE(c.page().rawInput.empty());
    }

    // 4. 对称反向验证：训练 "ne -> 呢" 20 次，验证不得劫持 "le -> 了"
    for (int n = 0; n < 20; ++n) {
        c.setContext("问句", "", fcitx5::ContextAvailability::Valid);
        feedKeys(c, "ne");
        const auto neRank = rank(c.page(), "呢");
        ASSERT_LT(neRank, c.page().items.size());
        ASSERT_TRUE(c.select(neRank));
        EXPECT_EQ(host.committed, "呢");
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "le");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "了") << "'了' must remain #1 for 'le'";
    EXPECT_EQ(c.page().items.front().fullPinyin, "le");
    const auto neRankAfter = rank(c.page(), "呢");
    EXPECT_GT(neRankAfter, 0u) << "'呢' must not occupy slot 0 for 'le'";
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "了");
}

TEST_F(CandidateRankingRegression, AP01_AP02_NormalLearningDoesNotPolluteFuzzyPair_NongLong) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. 训练 "long -> 龙" 20 次
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "long");
        const auto r = rank(c.page(), "龙");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    // 2. 验证 "nong" 输入的首选仍为正音 "弄"，不被高频 "龙" 劫持
    c.reset();
    type(c, "nong");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "弄");
    EXPECT_EQ(c.page().items.front().fullPinyin, "nong");
    EXPECT_GT(rank(c.page(), "龙"), 0u);
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "弄");

    // 3. 对称反向：训练 "nong -> 弄" 20 次，验证 "long" 首选不受影响
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "nong");
        const auto r = rank(c.page(), "弄");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "long");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "龙");
    EXPECT_GT(rank(c.page(), "弄"), 0u);
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "龙");
}

TEST_F(CandidateRankingRegression, AP01_AP02_NormalLearningDoesNotPolluteFuzzyPair_SiShi) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. 训练 "shi -> 是" 20 次（"是" 是极高频字）
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "shi");
        const auto r = rank(c.page(), "是");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    // 2. 验证 "si" 输入的首选为 "si" 组正音（如 "四" 或 "司"），绝不能被 "是" 劫持
    c.reset();
    type(c, "si");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_NE(c.page().items.front().text, "是") << "'是' must not hijack top slot for 'si'";
    EXPECT_EQ(c.page().items.front().fullPinyin, "si");
    EXPECT_GT(rank(c.page(), "是"), 0u);
    const auto topSiText = c.page().items.front().text;
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, topSiText);

    // 3. 对称反向：训练 "si -> 四" 20 次，验证 "shi" 首选仍为 "是"
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "si");
        const auto r = rank(c.page(), "四");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "shi");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "是");
    EXPECT_GT(rank(c.page(), "四"), 0u);
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "是");
}

TEST_F(CandidateRankingRegression, AP01_AP02_NormalLearningDoesNotPolluteExtendedSymmetricFuzzyPairs) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. ni / li 对
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "li");
        const auto r = rank(c.page(), "里");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "ni");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "你");
    EXPECT_GT(rank(c.page(), "里"), 0u);

    // 2. neng / leng 对
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "leng");
        const auto r = rank(c.page(), "冷");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "neng");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "能");
    EXPECT_GT(rank(c.page(), "冷"), 0u);

    // 3. zu / zhu 对
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "zhu");
        const auto r = rank(c.page(), "主");
        ASSERT_LT(r, c.page().items.size());
        ASSERT_TRUE(c.select(r));
    }
    ASSERT_TRUE(provider.flush());

    c.reset();
    type(c, "zu");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().fullPinyin, "zu");
    EXPECT_GT(rank(c.page(), "主"), 0u);
}

TEST_F(CandidateRankingRegression, AP01_AP02_ExplicitCorrectionIntentUnderSameRawElevatesSingleSyllable) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. 规范训练 "le -> 了" 20 次（使用有效上下文记录通用词频）
    for (int n = 0; n < 20; ++n) {
        c.setContext("完成", "", fcitx5::ContextAvailability::Valid);
        feedKeys(c, "le");
        ASSERT_TRUE(c.select(rank(c.page(), "了")));
    }
    ASSERT_TRUE(provider.flush());

    // 验证此时无纠正意图下，"ne" 保持首选 "呢"
    c.reset();
    type(c, "ne");
    EXPECT_EQ(c.page().items.front().text, "呢");
    c.handle({fcitx5::KeyKind::Escape});

    // 2. 用户在当前会话中，在 raw="ne" 下反复主动纠正选择 "了"（连续 4 次，同一会话不 reset）
    for (int n = 0; n < 4; ++n) {
        feedKeys(c, "ne");
        const auto leRank = rank(c.page(), "了");
        ASSERT_LT(leRank, c.page().items.size());
        ASSERT_TRUE(c.select(leRank));
        EXPECT_EQ(host.committed, "了");
    }

    // 3. 此时 recent_choice_bonus > 0 激活，根据产品纠正契约允许晋升 "了"
    feedKeys(c, "ne");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "了")
        << "Explicit repeated correction under raw 'ne' allows '了' to be promoted";
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "了");

    // 4. 会话重置/清空近期纠正意图后，重新回到保护正音的边界
    c.reset();
    type(c, "ne");
    EXPECT_EQ(c.page().items.front().text, "呢");
}

TEST_F(CandidateRankingRegression, AP01_AP02_MultiSyllableFuzzyVariantLearningPreserved) {
    pinyin::PinyinCandidateProvider provider(paths);
    Host host;
    fcitx5::ModernIMEController c(host, &provider);

    // 1. 未学习时，"zongwen" 不得凭空将 "中文" 置于首位
    type(c, "zongwen");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_NE(c.page().items.front().text, "中文");
    c.reset();

    // 2. 多音节经 20 次训练 "zongwen -> 中文"
    for (int n = 0; n < 20; ++n) {
        c.reset();
        type(c, "zongwen");
        const auto zwRank = rank(c.page(), "中文");
        ASSERT_LT(zwRank, c.page().items.size());
        ASSERT_TRUE(c.select(zwRank));
        EXPECT_EQ(host.committed, "中文");
    }
    ASSERT_TRUE(provider.flush());

    // 3. 验证多音节模糊音学习晋升机制完整保留（有效音节数 >= 2 契约生效）
    c.reset();
    type(c, "zongwen");
    ASSERT_FALSE(c.page().items.empty());
    EXPECT_EQ(c.page().items.front().text, "中文")
        << "Multi-syllable fuzzy preference 'zongwen -> 中文' must be preserved";
    ASSERT_TRUE(c.handle({fcitx5::KeyKind::Space}));
    EXPECT_EQ(host.committed, "中文");
}

TEST_F(CandidateRankingRegression, AP01_AP02_LifeCycleAndStorageIntegrity_ProviderReload) {
    // 1. 在一个 Provider 生命周期中训练 "le -> 了" 20 次
    {
        pinyin::PinyinCandidateProvider provider(paths);
        Host host;
        fcitx5::ModernIMEController c(host, &provider);
        for (int n = 0; n < 20; ++n) {
            c.setContext("完成", "", fcitx5::ContextAvailability::Valid);
            feedKeys(c, "le");
            ASSERT_TRUE(c.select(rank(c.page(), "了")));
        }
        ASSERT_TRUE(provider.flush());
    }

    // 2. 检查底层 SQLite 数据库：只有 ("了", "le") 正音键，绝无 ("了", "ne") 坏键
    {
        core::LearningStore store(paths.learningStore);
        ASSERT_TRUE(store.openReadOnly());
        const auto snap = store.snapshot();
        EXPECT_GE(snap->boostAt("了", "le", nowMs()), 1.8);
        EXPECT_EQ(snap->boostAt("了", "ne", nowMs()), 0.0);
    }

    // 3. 跨进程/跨实例模拟：使用全新 Provider/Controller 重新打开同一数据库
    {
        pinyin::PinyinCandidateProvider freshProvider(paths);
        Host freshHost;
        fcitx5::ModernIMEController freshC(freshHost, &freshProvider);

        // 验证正音 "le -> 了" 学习成果完好保留
        type(freshC, "le");
        ASSERT_FALSE(freshC.page().items.empty());
        EXPECT_EQ(freshC.page().items.front().text, "了");
        ASSERT_TRUE(freshC.handle({fcitx5::KeyKind::Space}));
        EXPECT_EQ(freshHost.committed, "了");
        freshC.reset();

        // 验证 "ne -> 呢" 在重开后依然受保护，未发生跨会话算法污染
        type(freshC, "ne");
        ASSERT_FALSE(freshC.page().items.empty());
        EXPECT_EQ(freshC.page().items.front().text, "呢");
        EXPECT_GT(rank(freshC.page(), "了"), 0u);
        ASSERT_TRUE(freshC.handle({fcitx5::KeyKind::Space}));
        EXPECT_EQ(freshHost.committed, "呢");
    }
}

#if defined(MODERNIME_HAS_FCITX5)
TEST_F(CandidateRankingRegression, CandidateBarLayout_MultiPageWithoutIndicator) {
    core::CandidatePage page;
    page.preedit = "qingbangwodakaiwenjian";
    for (std::size_t i = 0; i < 16; ++i) {
        core::CandidateItem item;
        item.text = "选项" + std::to_string(i);
        item.fullPinyin = "qing";
        item.sourceIndex = i;
        page.items.push_back(std::move(item));
    }
    page.pageBoundaries = {{0, 8}, {8, 16}};
    page.cursor = 0;

    const auto metrics = ui::CandidateBarMetrics::reference();
    const auto layout = ui::CandidateBarLayout::measure(page, metrics);
    page.pageBoundaries.clear();
    const auto plainLayout = ui::CandidateBarLayout::measure(page, metrics);
    ASSERT_FALSE(layout.candidates.empty());
    ASSERT_EQ(layout.candidates.size(), plainLayout.candidates.size());
    for (std::size_t i = 0; i < layout.candidates.size(); ++i) {
        const auto &candidate = layout.candidates[i];
        const auto &plain = plainLayout.candidates[i];
        EXPECT_EQ(candidate.displayText, plain.displayText);
        EXPECT_DOUBLE_EQ(candidate.bounds.x, plain.bounds.x);
        EXPECT_DOUBLE_EQ(candidate.bounds.width, plain.bounds.width);
        EXPECT_EQ(ui::hitTestCandidate(
                      layout, metrics,
                      candidate.bounds.x + candidate.bounds.width / 2.0,
                      candidate.bounds.y + candidate.bounds.height / 2.0),
                  static_cast<int>(i));
    }
}
#endif

} // namespace


