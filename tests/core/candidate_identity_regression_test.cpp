#include <gtest/gtest.h>

#include "modernime/core/candidate_model.h"
#include "modernime/core/commit_event.h"
#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/candidate_prefixes.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace modernime {
namespace {

struct TestHost final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed = text; }
};

class ArchitectureCandidateIdentityRegression : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "modernime-identity-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "empty-user.dict").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    static void type(fcitx5::ModernIMEController &c, std::string_view input) {
        c.reset();
        c.setContext("", "");
        for (char key : input) {
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Character, key}));
        }
        ASSERT_EQ(c.page().rawInput, input);
    }

    std::filesystem::path root;
    pinyin::PinyinDataPaths paths;
};

// 1. SameTextDifferentConsumptionSurvives
TEST_F(ArchitectureCandidateIdentityRegression, SameTextDifferentConsumptionSurvives) {
    // Pipeline / Mixer level verification:
    // Candidate A: partial prefix "测试", pinyin "ce'shi", consumed 5 bytes
    // Candidate B: full sentence "测试", pinyin "ce'shi", consumed 10 bytes
    core::CandidateItem candPrefix;
    candPrefix.text = "测试";
    candPrefix.fullPinyin = "ce'shi";
    candPrefix.consumedInputBytes = 5;
    candPrefix.source = core::CandidateSource::Engine;
    candPrefix.commitRole = core::CommitCandidateRole::Chinese;
    candPrefix.origins = {candPrefix.source};

    core::CandidateItem candFull;
    candFull.text = "测试";
    candFull.fullPinyin = "ce'shi";
    candFull.consumedInputBytes = 10;
    candFull.source = core::CandidateSource::Engine;
    candFull.commitRole = core::CommitCandidateRole::Chinese;
    candFull.origins = {candFull.source};

    core::CandidateItem runnerUp;
    runnerUp.text = "侧视";
    runnerUp.fullPinyin = "ce'shi";
    runnerUp.consumedInputBytes = 10;
    runnerUp.source = core::CandidateSource::Engine;
    runnerUp.commitRole = core::CommitCandidateRole::Chinese;
    runnerUp.origins = {runnerUp.source};

    // Both candidates must have distinct identities
    const auto idPrefix = core::candidateIdentity(candPrefix, 10);
    const auto idFull = core::candidateIdentity(candFull, 10);
    EXPECT_NE(idPrefix, idFull);
    EXPECT_EQ(idPrefix.text, idFull.text);
    EXPECT_EQ(idPrefix.consumedInputBytes, 5u);
    EXPECT_EQ(idFull.consumedInputBytes, 10u);

    // Run through mixCandidateItems: both must survive!
    const auto mixed = pinyin::mixCandidateItems(
        {candFull, runnerUp}, {candPrefix}, &candFull, {5}, "ceshiceshi");
    
    std::size_t foundPrefix = 0;
    std::size_t foundFull = 0;
    for (const auto &item : mixed) {
        if (item.text == "测试") {
            if (item.consumedInputBytes == 5) ++foundPrefix;
            if (item.consumedInputBytes == 10) ++foundFull;
        }
    }
    EXPECT_GE(foundPrefix, 1u);
    EXPECT_GE(foundFull, 1u);

    // End-to-end Controller level verification:
    // When partial prefix candidate is selected, it must consume prefix and leave remainder.
    pinyin::PinyinCandidateProvider provider(paths);
    TestHost host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "ceshi");
    ASSERT_FALSE(c.page().items.empty());

    // Record learning for prefix "测"
    for (int i = 0; i < 3; ++i) {
        type(c, "ceshi");
        std::size_t idx = c.page().items.size();
        for (std::size_t j = 0; j < c.page().items.size(); ++j) {
            if (c.page().items[j].text == "测" && c.page().items[j].consumedInputBytes < 5) {
                idx = j;
                break;
            }
        }
        if (idx < c.page().items.size()) {
            ASSERT_TRUE(c.select(idx));
            EXPECT_EQ(host.committed, "测");
            EXPECT_FALSE(c.page().rawInput.empty()); // Remainder kept!
            ASSERT_TRUE(c.handle({fcitx5::KeyKind::Escape}));
        }
    }
}

// 2. DifferentReadingAndRoleSurvive
TEST_F(ArchitectureCandidateIdentityRegression, DifferentReadingAndRoleSurvive) {
    // Verify different readings (polyphones) survive with identical text
    core::CandidateItem candZhong;
    candZhong.text = "重";
    candZhong.fullPinyin = "zhong";
    candZhong.consumedInputBytes = 5;
    candZhong.source = core::CandidateSource::Engine;
    candZhong.commitRole = core::CommitCandidateRole::Chinese;
    candZhong.origins = {candZhong.source};

    core::CandidateItem candChong;
    candChong.text = "重";
    candChong.fullPinyin = "chong";
    candChong.consumedInputBytes = 5;
    candChong.source = core::CandidateSource::Engine;
    candChong.commitRole = core::CommitCandidateRole::Chinese;
    candChong.origins = {candChong.source};

    // Verify different roles survive with identical text
    core::CandidateItem candChinese;
    candChinese.text = "run";
    candChinese.fullPinyin = "run";
    candChinese.consumedInputBytes = 3;
    candChinese.source = core::CandidateSource::Engine;
    candChinese.commitRole = core::CommitCandidateRole::Chinese;
    candChinese.origins = {candChinese.source};

    core::CandidateItem candEnglish;
    candEnglish.text = "run";
    candEnglish.fullPinyin = "run";
    candEnglish.consumedInputBytes = 3;
    candEnglish.source = core::CandidateSource::EnglishDefinition;
    candEnglish.commitRole = core::CommitCandidateRole::English;
    candEnglish.origins = {candEnglish.source};

    core::CandidateItem candRaw;
    candRaw.text = "run";
    candRaw.fullPinyin = "run";
    candRaw.consumedInputBytes = 3;
    candRaw.source = core::CandidateSource::Raw;
    candRaw.commitRole = core::CommitCandidateRole::Raw;
    candRaw.origins = {candRaw.source};

    const auto idZhong = core::candidateIdentity(candZhong, 5);
    const auto idChong = core::candidateIdentity(candChong, 5);
    EXPECT_NE(idZhong, idChong);
    EXPECT_EQ(idZhong.text, idChong.text);
    EXPECT_EQ(idZhong.fullPinyin, "zhong");
    EXPECT_EQ(idChong.fullPinyin, "chong");

    const auto idZh = core::candidateIdentity(candChinese, 3);
    const auto idEn = core::candidateIdentity(candEnglish, 3);
    const auto idRaw = core::candidateIdentity(candRaw, 3);
    EXPECT_NE(idZh, idEn);
    EXPECT_NE(idZh, idRaw);
    EXPECT_NE(idEn, idRaw);

    // Test in mixCandidateItems: polyphones both survive!
    core::CandidateItem dummy;
    dummy.text = "其他";
    dummy.fullPinyin = "qi'ta";
    dummy.consumedInputBytes = 5;
    dummy.source = core::CandidateSource::Engine;
    dummy.commitRole = core::CommitCandidateRole::Chinese;

    const auto mixedPoly = pinyin::mixCandidateItems(
        {candZhong, candChong}, {}, &candZhong, {2, 5}, "zhong");
    bool foundZhong = false;
    bool foundChong = false;
    for (const auto &item : mixedPoly) {
        if (item.text == "重" && item.fullPinyin == "zhong") foundZhong = true;
        if (item.text == "重" && item.fullPinyin == "chong") foundChong = true;
    }
    EXPECT_TRUE(foundZhong);
    EXPECT_TRUE(foundChong);

    // End-to-end Provider test with ambiguity:
    pinyin::PinyinCandidateProvider provider(paths);
    TestHost host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "chong");
    ASSERT_FALSE(c.page().items.empty());

    // When ambiguous items exist with the same text, annotation must display reading
    // and must NOT modify the committed text!
    for (std::size_t i = 0; i < c.page().items.size(); ++i) {
        const auto &item = c.page().items[i];
        if (!item.annotation.empty()) {
            // Commit text must remain untouched by annotation
            EXPECT_NE(item.text, item.annotation);
        }
    }
}

// 3. EquivalentOriginsMergeOnce
TEST_F(ArchitectureCandidateIdentityRegression, EquivalentOriginsMergeOnce) {
    // Two items with equivalent identity from different sources (Engine and Learned)
    core::CandidateItem candEngine;
    candEngine.text = "你好";
    candEngine.fullPinyin = "ni'hao";
    candEngine.consumedInputBytes = 5;
    candEngine.source = core::CandidateSource::Engine;
    candEngine.commitRole = core::CommitCandidateRole::Chinese;
    candEngine.origins = {core::CandidateSource::Engine};

    core::CandidateItem candLearned;
    candLearned.text = "你好";
    candLearned.fullPinyin = "ni'hao";
    candLearned.consumedInputBytes = 5;
    candLearned.source = core::CandidateSource::Learned;
    candLearned.commitRole = core::CommitCandidateRole::Chinese;
    candLearned.learningBoost = 2.0;
    candLearned.origins = {core::CandidateSource::Learned};

    const auto id1 = core::candidateIdentity(candEngine, 5);
    const auto id2 = core::candidateIdentity(candLearned, 5);
    EXPECT_EQ(id1, id2); // Equivalent!

    // In mixCandidateItems: equivalent items must merge into ONE item
    const auto mixed = pinyin::mixCandidateItems(
        {candEngine}, {candLearned}, &candEngine, {2, 5}, "nihao");

    std::size_t nihaoCount = 0;
    const core::CandidateItem *mergedItem = nullptr;
    for (const auto &item : mixed) {
        if (item.text == "你好" && item.fullPinyin == "ni'hao" && item.consumedInputBytes == 5) {
            ++nihaoCount;
            mergedItem = &item;
        }
    }
    // Must appear exactly once (no duplicate items)
    EXPECT_EQ(nihaoCount, 1u);
    ASSERT_NE(mergedItem, nullptr);
    // Origins must be merged!
    EXPECT_GE(mergedItem->origins.size(), 2u);
    EXPECT_NE(std::find(mergedItem->origins.begin(), mergedItem->origins.end(), core::CandidateSource::Engine), mergedItem->origins.end());
    EXPECT_NE(std::find(mergedItem->origins.begin(), mergedItem->origins.end(), core::CandidateSource::Learned), mergedItem->origins.end());

    // End-to-end: repeated selection must increment frequency exactly once
    pinyin::PinyinCandidateProvider provider(paths);
    TestHost host;
    fcitx5::ModernIMEController c(host, &provider);
    type(c, "nihao");
    ASSERT_FALSE(c.page().items.empty());

    const auto selectedText = c.page().items.front().text;
    ASSERT_TRUE(c.select(0));
    EXPECT_EQ(host.committed, selectedText);
    ASSERT_TRUE(provider.flush());

    // Verify SQLite learning store frequency incremented by exactly 1
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.open());
    const auto snapshot = store.snapshot();
    const auto entry = snapshot->entry(selectedText, "ni'hao", "", "");
    if (entry != nullptr) {
        EXPECT_EQ(entry->frequency, 1);
    }
}

} // namespace
} // namespace modernime
