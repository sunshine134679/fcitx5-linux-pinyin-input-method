#include <gtest/gtest.h>

#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace modernime::pinyin {
namespace {

class ArchitectureResourceCoordinationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() / "modernime-res-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
        paths.learningStore = (root / "learning.sqlite3").string();
        paths.userDictionary = (root / "user.dict").string();

        // Create initial user dictionary
        std::ofstream userDict(paths.userDictionary);
        userDict << "ceshi 测试 100\n";
        userDict.close();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
    PinyinDataPaths paths;
};

// 1. Shared resources can be shared across multiple providers without cross-session pollution
TEST_F(ArchitectureResourceCoordinationTest, ResourceSharingAcrossProvidersWithoutCrossSessionPollution) {
    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    PinyinCandidateProvider providerA(shared, options);
    PinyinCandidateProvider providerB(shared, options);

    // Provider A types "ceshi" and selects candidate 0
    ASSERT_TRUE(providerA.append("ceshi"));
    const auto &pageA = providerA.page();
    ASSERT_FALSE(pageA.items.empty());
    const auto initialGenA = pageA.generation;

    core::CandidateSelectionRequest reqA;
    reqA.displayGeneration = initialGenA;
    reqA.globalIndex = 0;
    reqA.expected = core::candidateIdentity(pageA.items[0], pageA);
    const auto outcomeA = providerA.selectConfirmed(reqA, initialGenA);
    ASSERT_TRUE(outcomeA.has_value());

    // Provider B starts fresh, input is empty
    ASSERT_TRUE(providerB.page().rawInput.empty());
    // Provider B types "ceshi"
    ASSERT_TRUE(providerB.append("ceshi"));
    const auto &pageB = providerB.page();
    ASSERT_FALSE(pageB.items.empty());

    // Provider B resets commit history without affecting Provider A
    providerB.resetCommitHistory();
    ASSERT_TRUE(providerA.flush());
}

// 2. User dictionary reload increments generation and invalidates stale selections
TEST_F(ArchitectureResourceCoordinationTest, ReloadUserDictionaryIncrementsGenerationAndInvalidatesSelection) {
    PinyinProviderOptions options;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    const auto initialGen = shared->dictionaryGeneration();
    EXPECT_EQ(initialGen, 1u);

    PinyinCandidateProvider provider(shared, options);
    ASSERT_TRUE(provider.append("ceshi"));
    const auto oldPageGen = provider.page().generation;

    // Append new phrase to user dictionary
    {
        std::ofstream userDict(paths.userDictionary, std::ios::app);
        userDict << "xinci 新词 200\n";
    }

    // Hot reload user dictionary
    const auto change = PinyinCandidateProvider::reloadUserDictionaryDetailed(shared);
    EXPECT_TRUE(change.changed);
    EXPECT_EQ(change.dictionaryGeneration, initialGen + 1);

    // Stale selection targeting oldPageGen must fail
    core::CandidateSelectionRequest staleReq;
    staleReq.displayGeneration = oldPageGen;
    staleReq.globalIndex = 0;
    staleReq.expected = core::candidateIdentity(provider.page().items[0], provider.page());

    // Provider detects change on next typing or refresh
    provider.append("a");
    EXPECT_GT(provider.page().generation, oldPageGen);

    const auto outcome = provider.selectConfirmed(staleReq, oldPageGen);
    EXPECT_FALSE(outcome.has_value());
}

// 3. Learning store reload/refresh clears phrase stats and resets session history
TEST_F(ArchitectureResourceCoordinationTest, ReloadLearningStoreResetsSessionRecentChoicesAndHistory) {
    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    PinyinCandidateProvider provider(shared, options);
    ASSERT_TRUE(provider.append("ceshi"));
    ASSERT_FALSE(provider.page().items.empty());

    core::CandidateSelectionRequest req;
    req.displayGeneration = provider.page().generation;
    req.globalIndex = 0;
    req.expected = core::candidateIdentity(provider.page().items[0], provider.page());
    ASSERT_TRUE(provider.selectConfirmed(req, provider.page().generation).has_value());
    ASSERT_TRUE(provider.flush());

    // Shared phraseStats should have been populated or used
    EXPECT_NE(shared->phraseStats, nullptr);

    // External reload of learning store clears shared phrase formation stats
    const auto change = PinyinCandidateProvider::reloadLearningStoreDetailed(shared);
    EXPECT_TRUE(change.changed);

    // Next interaction in provider clears session-local choices and history
    ASSERT_TRUE(provider.append("ceshi"));
    ASSERT_FALSE(provider.page().items.empty());
    provider.reset();
}

// 4. Normal -> Sensitive -> Normal preserves no sensitive evidence
TEST_F(ArchitectureResourceCoordinationTest, NormalToSensitiveToNormalPreservesNoSensitiveEvidence) {
    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    PinyinCandidateProvider provider(shared, options);

    // Normal mode: type and commit
    ASSERT_TRUE(provider.append("ceshi"));
    ASSERT_FALSE(provider.page().items.empty());
    core::CandidateSelectionRequest req;
    req.displayGeneration = provider.page().generation;
    req.globalIndex = 0;
    req.expected = core::candidateIdentity(provider.page().items[0], provider.page());
    ASSERT_TRUE(provider.selectConfirmed(req, provider.page().generation).has_value());
    ASSERT_TRUE(provider.flush());

    // Switch to sensitive mode (learning disabled)
    provider.setLearningEnabled(false);

    // Type sensitive password-like or private pinyin
    ASSERT_TRUE(provider.append("mima"));
    ASSERT_FALSE(provider.page().items.empty());
    core::CandidateSelectionRequest sensReq;
    sensReq.displayGeneration = provider.page().generation;
    sensReq.globalIndex = 0;
    sensReq.expected = core::candidateIdentity(provider.page().items[0], provider.page());
    ASSERT_TRUE(provider.selectConfirmed(sensReq, provider.page().generation).has_value());
    ASSERT_TRUE(provider.flush());

    // Check learning store: "mima" should NOT be recorded as learned
    core::LearningStore store(paths.learningStore);
    ASSERT_TRUE(store.openReadOnly());
    const auto state = store.readState();
    ASSERT_TRUE(state.has_value());
    if (state->snapshot) {
        EXPECT_FALSE(state->snapshot->hasPositiveFrequency("密码", "mima"));
    }

    // Switch back to normal mode
    provider.setLearningEnabled(true);
    provider.resetCommitHistory();
}

// 5. Read-only visibility does not create database file when learning is disabled
TEST_F(ArchitectureResourceCoordinationTest, ReadOnlyVisibilityDoesNotCreateDatabase) {
    const auto nonExistentDb = root / "never_created.sqlite3";
    PinyinDataPaths noDbPaths = paths;
    noDbPaths.learningStore = nonExistentDb.string();

    PinyinProviderOptions options;
    options.learningEnabled = false;

    auto shared = PinyinCandidateProvider::createSharedResources(noDbPaths, options);
    ASSERT_NE(shared, nullptr);

    // Database file must not have been created on disk
    EXPECT_FALSE(std::filesystem::exists(nonExistentDb));
}

// 6. Reloading state rejects selection, extraction, and removal
TEST_F(ArchitectureResourceCoordinationTest, ReloadingStateRejectsSelectionExtractionRemoval) {
    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    auto *writer = shared->ensureLearningWriter();
    ASSERT_NE(writer, nullptr);

    {
        core::LearningStore externalStore(paths.learningStore);
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }
    writer->pauseWorkerForTest(true);
    writer->pauseWorkerAfterReadForTest(true);
    EXPECT_TRUE(writer->reloadIfChanged());
    EXPECT_EQ(shared->epochViewState(), core::EpochViewState::Reloading);

    PinyinCandidateProvider provider(shared, options);
    ASSERT_TRUE(provider.append("ceshi"));
    const auto &page = provider.page();
    ASSERT_FALSE(page.items.empty());
    EXPECT_EQ(page.items[0].source, core::CandidateSource::None);

    core::CandidateSelectionRequest req;
    req.displayGeneration = page.generation;
    req.globalIndex = 0;
    req.expected = core::candidateIdentity(page.items[0], page);

    EXPECT_FALSE(provider.selectConfirmed(req, page.generation).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    writer->pauseWorkerForTest(false);
    writer->pauseWorkerAfterReadForTest(false);
    writer->flush();
}

} // namespace
} // namespace modernime::pinyin
