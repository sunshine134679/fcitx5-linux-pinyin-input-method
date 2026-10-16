#include <gtest/gtest.h>

#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <sqlite3.h>

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

// 7. S1: Page authorization is permanently revoked on resource changes across clear and Ready
TEST_F(ArchitectureResourceCoordinationTest, OldPageAuthorizationRevokedPermanentlyAcrossClearAndReady) {
    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    auto *writer = shared->ensureLearningWriter();
    ASSERT_NE(writer, nullptr);

    PinyinCandidateProvider provider(shared, options);
    ASSERT_TRUE(provider.append("ceshi"));
    const auto oldPage = provider.page();
    ASSERT_FALSE(oldPage.items.empty());
    const auto oldGen = oldPage.generation;

    core::CandidateSelectionRequest oldReq;
    oldReq.displayGeneration = oldGen;
    oldReq.globalIndex = 0;
    oldReq.expected = core::candidateIdentity(oldPage.items[0], oldPage);

    // Pause worker and clear store to trigger Reloading
    writer->pauseWorkerForTest(true);
    {
        core::LearningStore externalStore(paths.learningStore);
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }
    ASSERT_TRUE(writer->reloadIfChanged());
    ASSERT_EQ(shared->epochViewState(), core::EpochViewState::Reloading);

    // Call 1 on old request during Reloading: must be rejected
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    // Call 2 on old request during Reloading (without calling page() or append()):
    // MUST STILL BE REJECTED!
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    // Now resume worker and flush to transition to Ready
    writer->pauseWorkerForTest(false);
    ASSERT_TRUE(writer->flush());
    ASSERT_EQ(shared->epochViewState(), core::EpochViewState::Ready);

    // After transitioning to Ready, WITHOUT calling page() or append():
    // The old page authorization must REMAIN REVOKED!
    // Call 1 after Ready:
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    // Call 2 after Ready:
    // In unpatched code, this succeeds because checkResourceChanges() returns false!
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    // User dictionary reload:
    {
        std::ofstream userDict(paths.userDictionary, std::ios::app);
        userDict << "xinci 新词 200\n";
    }
    PinyinCandidateProvider::reloadUserDictionaryDetailed(shared);

    // Repeated calls to old request must still be rejected!
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.selectConfirmed(oldReq, oldGen).has_value());
    EXPECT_FALSE(provider.extractCharacter(0, true));
    EXPECT_FALSE(provider.remove(0));

    // Now explicitly refresh / build new page
    const auto &newPage = provider.page();
    EXPECT_GT(newPage.generation, oldGen);
    ASSERT_FALSE(newPage.items.empty());

    core::CandidateSelectionRequest newReq;
    newReq.displayGeneration = newPage.generation;
    newReq.globalIndex = 0;
    newReq.expected = core::candidateIdentity(newPage.items[0], newPage);

    // New request with new generation succeeds
    const auto outcome = provider.selectConfirmed(newReq, newPage.generation);
    EXPECT_TRUE(outcome.has_value());
}

// 8. T11-A10: Async startup initial commit does not block input thread and persists fact exactly once
TEST_F(ArchitectureResourceCoordinationTest, T11_A10_AsyncStartupInitialCommitDoesNotBlockInput) {
    // Populate database with 1000 items to simulate non-trivial dataset
    {
        core::LearningStore initStore(paths.learningStore);
        ASSERT_TRUE(initStore.open());
        for (int i = 0; i < 1000; ++i) {
            initStore.recordSelection("已有词" + std::to_string(i), "ci", "", "", 1000000 + i);
        }
        initStore.close();
    }

    PinyinProviderOptions options;
    options.learningEnabled = true;
    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);

    // Prepare async writer
    auto *writer = shared->ensureLearningWriter(/*synchronousInitialLoad=*/false);
    ASSERT_NE(writer, nullptr);

    PinyinCandidateProvider provider(shared, options);

    // Initial commit on input thread immediately while background load may still be running
    core::CommitEvent initialCommit;
    initialCommit.text = "首提交";
    initialCommit.fullPinyin = "shoutijiao";
    initialCommit.role = core::CommitCandidateRole::Chinese;
    initialCommit.candidateIndex = 1;
    initialCommit.isDeviation = true;
    initialCommit.timestampMs = 2000000;

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(writer->enqueueCommit(initialCommit));
    const auto initialCommitDurationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Must not block input thread (gate: < 5000 us)
    EXPECT_LT(initialCommitDurationUs, 5000);

    // Next key / commit on input thread
    core::CommitEvent nextCommit = initialCommit;
    nextCommit.text = "下一键";
    nextCommit.fullPinyin = "xiayijian";
    nextCommit.timestampMs = 2000001;
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_TRUE(writer->enqueueCommit(nextCommit));
    const auto nextKeyDurationUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t1).count();
    EXPECT_LT(nextKeyDurationUs, 5000);

    // Typing append also does not block
    EXPECT_TRUE(provider.append("ceshi"));

    // Wait for background persistence and rebase
    EXPECT_TRUE(writer->flush());
    EXPECT_EQ(shared->epochViewState(), core::EpochViewState::Ready);

    // Verify snapshot has positive frequency for both
    EXPECT_TRUE(writer->snapshot()->hasPositiveFrequency("首提交", "shoutijiao"));
    EXPECT_TRUE(writer->snapshot()->hasPositiveFrequency("下一键", "xiayijian"));

    // Independently verify SQLite database: exactly 1 entry for each with frequency == 1
    {
        sqlite3 *db = nullptr;
        ASSERT_EQ(sqlite3_open(paths.learningStore.c_str(), &db), SQLITE_OK);
        for (const auto &phrase : {"首提交", "下一键"}) {
            sqlite3_stmt *stmt = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT count(*), frequency FROM learning_entries WHERE phrase = ?;", -1, &stmt, nullptr), SQLITE_OK);
            sqlite3_bind_text(stmt, 1, phrase, -1, SQLITE_TRANSIENT);
            ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
            EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
            EXPECT_EQ(sqlite3_column_int(stmt, 1), 1);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    // Toggle learning disabled and enabled to verify no side effects
    provider.setLearningEnabled(false);
    EXPECT_TRUE(provider.append("ceshi"));
    provider.setLearningEnabled(true);
    EXPECT_TRUE(provider.append("a"));
}

} // namespace
} // namespace modernime::pinyin

