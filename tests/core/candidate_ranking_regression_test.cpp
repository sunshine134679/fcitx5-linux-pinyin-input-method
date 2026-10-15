#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/recent_choice.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>
#include <unistd.h>

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
    std::filesystem::path root;
    pinyin::PinyinDataPaths paths;
};

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
    core::CandidateItem full{.text="你好世界", .fullPinyin="ni'hao'shi'jie",
                             .consumedInputBytes=11, .learningBoost=3.9};
    const auto mixed = pinyin::mixCandidateItems({full}, {}, &full, {2,5,8}, "nihaoshijie");
    for (const auto &item : mixed) {
        if (item.consumedInputBytes < 11) { EXPECT_DOUBLE_EQ(item.learningBoost, 0.0); }
    }
}

TEST(CandidateCompositionRegression, StrongPersonalPrefixKeepsFullSentenceQuota) {
    std::vector<core::CandidateItem> full{
        {.text="甲乙丙丁", .fullPinyin="jia'yi'bing'ding", .consumedInputBytes=13},
        {.text="甲乙丙钉", .fullPinyin="jia'yi'bing'ding", .consumedInputBytes=13},
        {.text="甲乙丙订", .fullPinyin="jia'yi'bing'ding", .consumedInputBytes=13}};
    std::vector<core::CandidateItem> partial{
        {.text="甲", .fullPinyin="jia", .source=core::CandidateSource::Learned,
         .consumedInputBytes=3, .learningBoost=3.9}};
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
} // namespace
