#include "modernime/core/learning_store.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using modernime::core::CommitCandidateRole;
using modernime::core::CommitEvent;
using modernime::core::LearningStore;
using modernime::pinyin::PinyinCandidateProvider;
using modernime::pinyin::PinyinDataPaths;

class PersonalizationProviderRegression : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "modernime-provider-regression-XXXXXX").string();
        const auto *created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "user.dict").string();
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    static std::size_t indexOf(const PinyinCandidateProvider &provider,
                               std::string_view text) {
        const auto &items = provider.page().items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].text == text) return i;
        }
        return items.size();
    }
    static void select(PinyinCandidateProvider &provider, std::string_view input,
                       std::string_view text) {
        ASSERT_TRUE(provider.append(input));
        const auto index = indexOf(provider, text);
        ASSERT_LT(index, provider.page().items.size()) << input << " / " << text;
        ASSERT_TRUE(provider.select(index));
    }
    static void nameRound(PinyinCandidateProvider &provider) {
        select(provider, "wang", "王");
        select(provider, "shao", "绍");
        select(provider, "long", "龙");
        provider.resetCommitHistory();
    }
    bool learnedName() {
        LearningStore store(paths.learningStore);
        EXPECT_TRUE(store.open());
        return store.snapshot()->hasPositiveFrequency("王绍龙", "wangshaolong");
    }
    std::filesystem::path root;
    PinyinDataPaths paths;
};

// Disabling learning must gate shared phrase statistics, not only disk writes.
TEST_F(PersonalizationProviderRegression, SensitiveRoundsDoNotContributeToSharedFormation) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    PinyinCandidateProvider sensitive(shared);
    sensitive.setLearningEnabled(false);
    nameRound(sensitive);
    nameRound(sensitive);
    PinyinCandidateProvider normal(shared);
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_FALSE(learnedName());
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_FALSE(learnedName());
    nameRound(normal);
    ASSERT_TRUE(normal.flush());
    EXPECT_TRUE(learnedName());
}

TEST_F(PersonalizationProviderRegression, LearningToggleClearsTransientSequenceButKeepsNormalStats) {
    PinyinCandidateProvider provider(paths);
    nameRound(provider);
    nameRound(provider);
    select(provider, "wang", "王");
    provider.setLearningEnabled(false);
    provider.setLearningEnabled(true);
    select(provider, "shao", "绍");
    select(provider, "long", "龙");
    provider.resetCommitHistory();
    ASSERT_TRUE(provider.flush());
    EXPECT_FALSE(learnedName());
    nameRound(provider);
    ASSERT_TRUE(provider.flush());
    EXPECT_TRUE(learnedName());
}

TEST_F(PersonalizationProviderRegression, SuppressedNativeCandidateStaysHiddenAcrossSharedAndRestartedProviders) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    {
        PinyinCandidateProvider provider(shared);
        ASSERT_TRUE(provider.append("nihao"));
        const auto index = indexOf(provider, "你好");
        ASSERT_LT(index, provider.page().items.size());
        ASSERT_TRUE(provider.remove(index));
        ASSERT_TRUE(provider.flush());
        EXPECT_EQ(indexOf(provider, "你好"), provider.page().items.size());
    }
    {
        PinyinCandidateProvider provider(shared);
        ASSERT_TRUE(provider.append("nihao"));
        EXPECT_EQ(indexOf(provider, "你好"), provider.page().items.size());
    }
    shared.reset();
    PinyinCandidateProvider restarted(paths);
    ASSERT_TRUE(restarted.append("nihao"));
    EXPECT_EQ(indexOf(restarted, "你好"), restarted.page().items.size());
    CommitEvent explicitCommit;
    explicitCommit.text = "你好";
    explicitCommit.fullPinyin = "ni'hao";
    explicitCommit.rawKeystrokes = "nihao";
    explicitCommit.role = CommitCandidateRole::Chinese;
    explicitCommit.timestampMs = 1700000000000LL;
    restarted.onCommitEvent(explicitCommit);
    ASSERT_TRUE(restarted.flush());
    restarted.reset();
    ASSERT_TRUE(restarted.append("nihao"));
    EXPECT_LT(indexOf(restarted, "你好"), restarted.page().items.size());
}

TEST_F(PersonalizationProviderRegression, ExplicitSelectionInSharedContextRestoresSuppressedCandidateEverywhere) {
    auto shared = PinyinCandidateProvider::createSharedResources(paths, {});
    PinyinCandidateProvider first(shared);
    ASSERT_TRUE(first.append("nihao"));
    const auto index = indexOf(first, "你好");
    ASSERT_LT(index, first.page().items.size());
    ASSERT_TRUE(first.remove(index));
    PinyinCandidateProvider second(shared);
    CommitEvent selection;
    selection.text = "你好";
    selection.fullPinyin = "ni'hao";
    selection.rawKeystrokes = "nihao";
    selection.role = CommitCandidateRole::Chinese;
    selection.timestampMs = 1700000000000LL;
    second.onCommitEvent(selection);
    ASSERT_TRUE(second.flush());
    first.reset();
    ASSERT_TRUE(first.append("nihao"));
    EXPECT_LT(indexOf(first, "你好"), first.page().items.size());
}

TEST_F(PersonalizationProviderRegression, StaticEnglishSuppressionAppliesImmediatelyAndAtOtherPrefixesAfterRestart) {
    {
        PinyinCandidateProvider provider(paths);
        ASSERT_TRUE(provider.append("gara"));
        const auto index = indexOf(provider, "garage");
        ASSERT_LT(index, provider.page().items.size());
        ASSERT_TRUE(provider.remove(index));
        EXPECT_EQ(indexOf(provider, "garage"), provider.page().items.size());
        ASSERT_TRUE(provider.flush());
    }
    PinyinCandidateProvider restarted(paths);
    ASSERT_TRUE(restarted.append("garag"));
    EXPECT_EQ(indexOf(restarted, "garage"), restarted.page().items.size());
}

TEST_F(PersonalizationProviderRegression, EnglishCompletionsLearnOneFullWordKeyAcrossPrefixes) {
    PinyinCandidateProvider provider(paths);
    ASSERT_TRUE(provider.append("gara"));
    const auto first = indexOf(provider, "garage");
    ASSERT_LT(first, provider.page().items.size());
    EXPECT_EQ(provider.page().items[first].consumedInputBytes, 4u);
    ASSERT_TRUE(provider.select(first));
    select(provider, "garag", "garage");
    ASSERT_TRUE(provider.flush());
    LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    const auto *entry = snapshot->entry("garage", "garage", "", "");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->frequency, 2);
    EXPECT_EQ(snapshot->entry("garage", "gara", "", ""), nullptr);
    EXPECT_EQ(snapshot->entry("garage", "garag", "", ""), nullptr);
    const auto recalled = snapshot->recallEnglishWords("gar");
    ASSERT_FALSE(recalled.empty());
    EXPECT_EQ(recalled.front().text, "garage");
}
} // namespace
