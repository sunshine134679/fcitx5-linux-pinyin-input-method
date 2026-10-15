#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {
using modernime::fcitx5::KeyKind;
using modernime::core::LearningSnapshot;

struct Host final : modernime::fcitx5::EngineHost {
    void publishPage(const modernime::core::CandidatePage &) override {}
    void commit(std::string_view) override {}
};

class PersonalizationControllerRegression : public testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "modernime-controller-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        modernime::pinyin::PinyinDataPaths paths;
        paths.learningStore = (root / "learning.sqlite3").string();
        provider = std::make_unique<modernime::pinyin::PinyinCandidateProvider>(paths);
        controller = std::make_unique<modernime::fcitx5::ModernIMEController>(host, provider.get());
    }
    void TearDown() override {
        controller.reset();
        provider.reset();
        std::filesystem::remove_all(root);
    }
    void type(std::string_view input) {
        for (char c : input) {
            ASSERT_TRUE(controller->handle({KeyKind::Character, c, 0}));
        }
    }
    void commit(std::string_view input, std::string_view text) {
        type(input);
        const auto &items = controller->page().items;
        std::size_t index = 0;
        while (index < items.size() && items[index].text != text) { ++index; }
        ASSERT_LT(index, items.size());
        ASSERT_TRUE(controller->select(index));
    }
    std::shared_ptr<const LearningSnapshot> snapshot() {
        EXPECT_TRUE(provider->flush());
        modernime::core::LearningStore store(root / "learning.sqlite3");
        EXPECT_TRUE(store.openReadOnly());
        return store.snapshot();
    }
    Host host;
    std::filesystem::path root;
    std::unique_ptr<modernime::pinyin::PinyinCandidateProvider> provider;
    std::unique_ptr<modernime::fcitx5::ModernIMEController> controller;
};

TEST_F(PersonalizationControllerRegression, CursorMovementNeverLearnsDeletion) {
    commit("nihao", "你好");
    controller->setContext("你好", "");
    controller->setContext("你", "好");
    controller->setContext("", "你好");
    type("n");
    const auto state = snapshot();
    EXPECT_FALSE(state->hasPositiveFrequency("你", "ni"));
    const auto *word = state->entry("你好", "nihao", "", "");
    ASSERT_NE(word, nullptr);
    EXPECT_EQ(word->negativeFeedback, 0);
}

TEST_F(PersonalizationControllerRegression, FullDeletionDiscardsIntermediateSingleCharacter) {
    commit("nihao", "你好");
    controller->setContext("你好", "");
    controller->setContext("你", "");
    EXPECT_FALSE(snapshot()->hasPositiveFrequency("你", "ni"));
    controller->setContext("", "");
    type("s");
    const auto state = snapshot();
    EXPECT_FALSE(state->hasPositiveFrequency("你", "ni"));
    const auto *word = state->entry("你好", "nihao", "", "");
    ASSERT_NE(word, nullptr);
    EXPECT_EQ(word->negativeFeedback, 1);
}

TEST_F(PersonalizationControllerRegression, ValidEmptyContextReplacesFallbackAfterDeletion) {
    commit("nihao", "你好");
    controller->setContext("你好", "");
    controller->setContext("你", "");
    controller->setContext("", "");
    commit("shijie", "世界");
    const auto state = snapshot();
    ASSERT_NE(state->entry("世界", "shijie", "", ""), nullptr);
    for (const auto &entry : state->entries()) {
        if (entry.phrase == "世界") { EXPECT_TRUE(entry.contextBefore.empty()); }
    }
}

TEST_F(PersonalizationControllerRegression, RetainedSingleCharacterIsConfirmedBeforeNextInputRanking) {
    commit("jieshao", "介绍");
    controller->setContext("绍", "");
    EXPECT_FALSE(snapshot()->hasPositiveFrequency("绍", "shao"));
    type("shao");
    const auto state = snapshot();
    const auto *retained = state->entry("绍", "shao", "", "");
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->frequency, 3);
    ASSERT_FALSE(controller->page().items.empty());
    EXPECT_EQ(controller->page().items.front().text, "绍");
}

TEST_F(PersonalizationControllerRegression, SensitiveTransitionDiscardsPendingDeletion) {
    commit("hualongdianjing", "画龙点睛");
    controller->setContext("画龙", "");
    controller->setSensitive(true);
    controller->setSensitive(false);
    const auto state = snapshot();
    EXPECT_FALSE(state->hasPositiveFrequency("画龙", "hualong"));
    const auto *word = state->entry("画龙点睛", "hualongdianjing", "", "");
    ASSERT_NE(word, nullptr);
    EXPECT_EQ(word->negativeFeedback, 0);
}

TEST_F(PersonalizationControllerRegression, MiddleCharacterDeletionStillReceivesStrongPromotion) {
    commit("hualongdianjing", "画龙点睛");
    controller->setContext("画龙点", "");
    controller->setContext("画龙", "");
    controller->setContext("龙", "");
    type("long");
    const auto state = snapshot();
    const auto *retained = state->entry("龙", "long", "", "");
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->frequency, 3);
    ASSERT_FALSE(controller->page().items.empty());
    EXPECT_EQ(controller->page().items.front().text, "龙");
}
TEST_F(PersonalizationControllerRegression, BoundedSurroundingWindowKeepsRealDeletionEvidence) {
    const std::string prefix(32, 'x');
    controller->setContext(prefix, "");
    commit("nihao", "你好");
    controller->setContext(prefix.substr(2) + "你好", "");
    controller->setContext(prefix.substr(1) + "你", "");
    type("ni");
    const auto state = snapshot();
    const auto *retained = state->entry("你", "ni", "", "");
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(retained->frequency, 3);
}

TEST_F(PersonalizationControllerRegression, UnavailableContextUsesOnlyContinuousCommitFallback) {
    using modernime::fcitx5::ContextAvailability;
    controller->setContext("", "", ContextAvailability::Unavailable);
    commit("ni", "你");
    controller->setContext("", "", ContextAvailability::Unavailable);
    commit("hao", "好");
    auto state = snapshot();
    const auto *word = state->entry("好", "hao", "你", "");
    ASSERT_NE(word, nullptr);
    EXPECT_EQ(word->contextBefore, "你");
    controller->handle({KeyKind::Backspace, 0, 0});
    controller->setContext("", "", ContextAvailability::Unavailable);
    commit("shijie", "世界");
    state = snapshot();
    ASSERT_NE(state->entry("世界", "shijie", "", ""), nullptr);
    for (const auto &entry : state->entries()) {
        if (entry.phrase == "世界") { EXPECT_TRUE(entry.contextBefore.empty()); }
    }
}

TEST_F(PersonalizationControllerRegression, InvalidatedContextDiscardsPendingRemainder) {
    using modernime::fcitx5::ContextAvailability;
    commit("nihao", "你好");
    controller->setContext("你", "");
    controller->setContext("", "", ContextAvailability::Invalidated);
    commit("shijie", "世界");
    const auto state = snapshot();
    EXPECT_FALSE(state->hasPositiveFrequency("你", "ni"));
    ASSERT_NE(state->entry("世界", "shijie", "", ""), nullptr);
    for (const auto &entry : state->entries()) {
        if (entry.phrase == "世界") { EXPECT_TRUE(entry.contextBefore.empty()); }
    }
}

TEST_F(PersonalizationControllerRegression, HostNavigationDoesNotConfirmPendingRemainder) {
    commit("nihao", "你好");
    controller->setContext("你", "");
    EXPECT_FALSE(controller->handle({KeyKind::PreviousClipboardItem, 0, 0}));
    EXPECT_FALSE(snapshot()->hasPositiveFrequency("你", "ni"));
    controller->setContext("另一处", "");
    type("n");
    EXPECT_FALSE(snapshot()->hasPositiveFrequency("你", "ni"));
}

TEST_F(PersonalizationControllerRegression, SensitiveCommitsNeverBecomeNormalFallbackContext) {
    controller->setSensitive(true);
    commit("mima", "密码");
    controller->setSensitive(false);
    commit("shijie", "世界");
    const auto state = snapshot();
    ASSERT_NE(state->entry("世界", "shijie", "", ""), nullptr);
    for (const auto &entry : state->entries()) {
        if (entry.phrase == "世界") { EXPECT_TRUE(entry.contextBefore.empty()); }
    }
}

} // namespace
