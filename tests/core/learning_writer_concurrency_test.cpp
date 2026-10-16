#include "modernime/core/candidate_model.h"
#include "modernime/core/commit_event.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/pinyin_match.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"

#include <gtest/gtest.h>

#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using modernime::core::CandidateSelectionRequest;
using modernime::core::CandidateSource;
using modernime::core::CommitCandidateRole;
using modernime::core::CommitEvent;
using modernime::core::EpochViewState;
using modernime::core::LearningStore;
using modernime::core::LearningWriter;
using modernime::core::PinyinMatchPolicy;
using modernime::pinyin::PinyinCandidateProvider;
using modernime::pinyin::PinyinDataPaths;
using modernime::pinyin::PinyinProviderOptions;

class ArchitectureWriterRegression : public testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-writer-regression-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::filesystem::path dbPath() const {
        return root_ / "learning.sqlite3";
    }

    std::filesystem::path lmdbPath() const {
        return root_ / "learning.lmdb";
    }

    std::filesystem::path root_;
};

TEST_F(ArchitectureWriterRegression, CommitRacesWithPublication) {
    LearningWriter writer(dbPath());

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> commitsDone{0};

    // Keep an old snapshot reference to verify immutability
    auto baselineSnapshot = writer.snapshot();
    ASSERT_NE(baselineSnapshot, nullptr);
    const auto beforeEntries = baselineSnapshot->entries().size();

    // 4 concurrent producer threads repeatedly enqueueing commits
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&, t] {
            std::uint64_t counter = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                CommitEvent event;
                event.text = "测试" + std::to_string(t) + "_" + std::to_string(counter % 20);
                event.fullPinyin = "ceshi";
                event.role = CommitCandidateRole::Chinese;
                event.candidateIndex = 1;
                event.isDeviation = true;
                event.timestampMs = 1000 + static_cast<std::int64_t>(counter);

                writer.enqueueCommit(event);
                commitsDone.fetch_add(1, std::memory_order_relaxed);
                ++counter;
                if (counter % 50 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 2 concurrent reader threads taking snapshots
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = writer.snapshot();
                ASSERT_NE(snap, nullptr);
                // Accessing snapshot methods must be safe and never race
                snap->hasPositiveFrequency("ceshi", "ceshi");
                std::this_thread::yield();
            }
        });
    }

    // Let threads run for 300ms
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);

    for (auto &p : producers) {
        p.join();
    }
    for (auto &r : readers) {
        r.join();
    }

    EXPECT_GT(commitsDone.load(), 100u);
    EXPECT_TRUE(writer.flush());

    // Verify baseline snapshot remained immutable
    EXPECT_EQ(baselineSnapshot->entries().size(), beforeEntries);

    auto finalSnapshot = writer.snapshot();
    ASSERT_NE(finalSnapshot, nullptr);
}

TEST_F(ArchitectureWriterRegression, DecisionUsesRefreshedRevision) {
    LearningWriter writer(dbPath());

    // "是" / "shi" is a core default Chinese candidate
    ASSERT_TRUE(PinyinMatchPolicy::isCoreChineseWord("是", "shi"));

    CommitEvent coreDefaultEvent;
    coreDefaultEvent.text = "是";
    coreDefaultEvent.fullPinyin = "shi";
    coreDefaultEvent.role = CommitCandidateRole::Chinese;
    coreDefaultEvent.candidateIndex = 0;
    coreDefaultEvent.isDeviation = false;
    coreDefaultEvent.isPartial = false;
    coreDefaultEvent.contextBefore = "";
    coreDefaultEvent.contextAfter = "";
    coreDefaultEvent.timestampMs = 1000;

    // 1. Without prior history, default core Chinese candidate must be skipped
    EXPECT_TRUE(writer.enqueueCommit(coreDefaultEvent));
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("是", "shi"));

    // 2. Explicit selection learns "是"
    EXPECT_TRUE(writer.enqueueSelection("是", "shi", "", "", 2000));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("是", "shi"));
    EXPECT_TRUE(writer.flush());

    // 3. Now default core Chinese candidate has positive frequency, so it is recorded
    EXPECT_TRUE(writer.enqueueCommit(coreDefaultEvent));
    EXPECT_TRUE(writer.flush());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("是", "shi"));

    // 4. External clear occurs via separate LearningStore
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    // 5. Immediately commit default core candidate without history.
    // Decision must observe refreshed reset revision and NOT use old snapshot!
    EXPECT_TRUE(writer.enqueueCommit(coreDefaultEvent));

    // The event should have been skipped under the new generation
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("是", "shi"));

    EXPECT_TRUE(writer.flush());
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("是", "shi"));
}

TEST_F(ArchitectureWriterRegression, FirstCommitAfterClearDuringRebase) {
    LearningWriter writer(dbPath());

    EXPECT_TRUE(writer.enqueueSelection("北京", "beijing", "", "", 1000));
    EXPECT_TRUE(writer.flush());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));
    const auto initialRevision = writer.resetRevision();

    // External clear occurs
    writer.pauseWorkerForTest(true);
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    // Observing clear bumps revision and enters Reloading
    EXPECT_TRUE(writer.reloadIfChanged());
    EXPECT_EQ(writer.epochViewState(), EpochViewState::Reloading);
    EXPECT_GT(writer.resetRevision(), initialRevision);
    const auto newRevision = writer.resetRevision();

    // Commit arrives during rebase (while in Reloading)
    CommitEvent commitShanghai;
    commitShanghai.text = "上海";
    commitShanghai.fullPinyin = "shanghai";
    commitShanghai.role = CommitCandidateRole::Chinese;
    commitShanghai.candidateIndex = 1;
    commitShanghai.isDeviation = true;
    commitShanghai.timestampMs = 2000;

    EXPECT_TRUE(writer.enqueueCommit(commitShanghai));

    // Resume worker to complete rebase
    writer.pauseWorkerForTest(false);

    // Finish rebase and persistence
    EXPECT_TRUE(writer.flush());
    EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);
    EXPECT_EQ(writer.resetRevision(), newRevision);

    // Old vocabulary must NOT be resurrected; new event was tagged with new reset revision
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("上海", "shanghai"));
}

TEST_F(ArchitectureWriterRegression, NewEpochSuppressionNotExposedWhileLoading) {
    PinyinDataPaths paths;
    paths.learningStore = dbPath().string();
    paths.userDictionary = (root_ / "user.dict").string();
    {
        std::ofstream userDict(paths.userDictionary);
        userDict << "ceshi 测试 100\n";
    }

    PinyinProviderOptions options;
    options.learningEnabled = true;

    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);
    auto *writer = shared->ensureLearningWriter();
    ASSERT_NE(writer, nullptr);

    // Suppress "测试"
    EXPECT_TRUE(writer->enqueueSuppression("测试", "ceshi"));
    EXPECT_TRUE(writer->flush());
    EXPECT_TRUE(writer->snapshot()->isSuppressed("测试", "ceshi"));

    PinyinCandidateProvider provider(shared, options);
    ASSERT_TRUE(provider.append("ceshi"));
    for (const auto &item : provider.page().items) {
        EXPECT_NE(item.text, "测试");
    }

    // External clear occurs
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    // Pause worker to deterministically observe the Reloading transition
    writer->pauseWorkerForTest(true);

    // Revision bump enters Reloading state
    EXPECT_TRUE(writer->reloadIfChanged());
    EXPECT_EQ(writer->epochViewState(), EpochViewState::Reloading);

    // Provider shows "正在更新候选" (CandidateSource::None)
    const auto &loadingPage = provider.page();
    ASSERT_EQ(loadingPage.items.size(), 1u);
    EXPECT_EQ(loadingPage.items[0].text, "正在更新候选");
    EXPECT_EQ(loadingPage.items[0].source, CandidateSource::None);

    // Candidate selection is rejected during reloading
    CandidateSelectionRequest req;
    req.displayGeneration = loadingPage.generation;
    req.globalIndex = 0;
    req.expected = modernime::core::candidateIdentity(loadingPage.items[0], loadingPage);
    const auto outcome = provider.selectConfirmed(req, loadingPage.generation);
    EXPECT_FALSE(outcome.has_value());

    // Resume worker to allow rebase to finish
    writer->pauseWorkerForTest(false);

    // Flush writer to complete rebase to Ready
    EXPECT_TRUE(writer->flush());
    EXPECT_EQ(writer->epochViewState(), EpochViewState::Ready);

    // Transition restores candidate list, old unverified suppression is gone
    const auto &readyPage = provider.page();
    ASSERT_FALSE(readyPage.items.empty());
    EXPECT_NE(readyPage.items[0].text, "正在更新候选");
    bool foundRestored = false;
    for (const auto &item : readyPage.items) {
        if (item.text == "测试") {
            foundRestored = true;
            break;
        }
    }
    EXPECT_TRUE(foundRestored);
}

TEST_F(ArchitectureWriterRegression, QueueCapacityEnforcedDuringReloadingAndStorageFailure) {
    LearningWriter writer(dbPath());

    // --- Part A: Reloading queue bound to 100, exactly 100 latest items preserved in DB ---
    writer.pauseWorkerForTest(true);
    struct Resume { LearningWriter &w; ~Resume() { w.pauseWorkerForTest(false); } } resume{writer};

    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }
    ASSERT_TRUE(writer.reloadIfChanged());
    ASSERT_EQ(writer.epochViewState(), EpochViewState::Reloading);

    // Enqueue 150 items under Reloading
    for (int i = 0; i < 150; ++i) {
        CommitEvent event;
        event.text = "上海" + std::to_string(i);
        event.fullPinyin = "shanghai";
        event.role = CommitCandidateRole::Chinese;
        event.candidateIndex = 1;
        event.isDeviation = true;
        event.timestampMs = 2000 + i;
        writer.enqueueCommit(event);
        EXPECT_LE(writer.queuedEventsCount(), 100u);
    }
    EXPECT_LE(writer.queuedEventsCount(), 100u);

    // Resume worker and persist
    writer.pauseWorkerForTest(false);
    EXPECT_TRUE(writer.flush());
    EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);

    // Independently inspect SQLite database
    {
        sqlite3 *db = nullptr;
        ASSERT_EQ(sqlite3_open(dbPath().c_str(), &db), SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT count(*) FROM learning_entries;", -1, &stmt, nullptr), SQLITE_OK);
        ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
        const int totalCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        EXPECT_EQ(totalCount, 100);

        // Earliest 50 dropped: "上海0".."上海49"
        for (int i = 0; i < 50; ++i) {
            std::string phrase = "上海" + std::to_string(i);
            sqlite3_stmt *check = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT count(*) FROM learning_entries WHERE phrase = ?;", -1, &check, nullptr), SQLITE_OK);
            sqlite3_bind_text(check, 1, phrase.c_str(), -1, SQLITE_TRANSIENT);
            ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
            EXPECT_EQ(sqlite3_column_int(check, 0), 0) << "Expected " << phrase << " to be dropped";
            sqlite3_finalize(check);
        }

        // Latest 100 preserved: "上海50".."上海149"
        for (int i = 50; i < 150; ++i) {
            std::string phrase = "上海" + std::to_string(i);
            sqlite3_stmt *check = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT frequency FROM learning_entries WHERE phrase = ?;", -1, &check, nullptr), SQLITE_OK);
            ASSERT_EQ(sqlite3_bind_text(check, 1, phrase.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
            ASSERT_EQ(sqlite3_step(check), SQLITE_ROW) << "Expected " << phrase << " to be present";
            EXPECT_EQ(sqlite3_column_int(check, 0), 1);
            sqlite3_finalize(check);
        }
        sqlite3_close(db);
    }

    // --- Part B: Storage failure boundedness ---
    sqlite3 *faultInjector = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath().c_str(), &faultInjector), SQLITE_OK);
    const char *trigger =
        "CREATE TRIGGER fail_storage_write BEFORE INSERT ON learning_entries "
        "BEGIN SELECT RAISE(ABORT, 'injected storage failure'); END;";
    char *err = nullptr;
    ASSERT_EQ(sqlite3_exec(faultInjector, trigger, nullptr, nullptr, &err), SQLITE_OK);
    sqlite3_free(err);

    for (int i = 0; i < 150; ++i) {
        writer.enqueueSelection("故障词" + std::to_string(i), "ci", "", "", 4000 + i);
    }

    // Flush will attempt to persist, fail due to trigger, and return false
    EXPECT_FALSE(writer.flush());
    EXPECT_LE(writer.queuedEventsCount(), 100u);
    EXPECT_GT(writer.queuedEventsCount(), 0u);

    // Drop trigger, persistence recovers
    sqlite3_exec(faultInjector, "DROP TRIGGER IF EXISTS fail_storage_write;", nullptr, nullptr, nullptr);
    sqlite3_close(faultInjector);

    EXPECT_TRUE(writer.flush());
}

TEST_F(ArchitectureWriterRegression, EnableLearningDoesNotConstructWriterOnInputThread) {
    PinyinDataPaths paths;
    paths.learningStore = dbPath().string();
    paths.userDictionary = (root_ / "user.dict").string();
    {
        std::ofstream userDict(paths.userDictionary);
        userDict << "ceshi 测试 100\n";
    }

    PinyinProviderOptions options;
    options.learningEnabled = false;

    auto shared = PinyinCandidateProvider::createSharedResources(paths, options);
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->learning, nullptr);

    PinyinCandidateProvider provider(shared, options);

    const auto start = std::chrono::steady_clock::now();
    provider.setLearningEnabled(true);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    EXPECT_LT(elapsed.count(), 5000);

    auto *writer = shared->ensureLearningWriter();
    ASSERT_NE(writer, nullptr);
    EXPECT_NE(shared->learning, nullptr);
}

TEST_F(ArchitectureWriterRegression, CommitRoleMatrixParityBetweenNormalAndReloading) {
    auto runRoleMatrixTest = [&](bool underReloading) {
        auto testDbPath = root_ / (underReloading ? "matrix_reloading.sqlite3" : "matrix_normal.sqlite3");
        LearningWriter writer(testDbPath);

        if (underReloading) {
            writer.pauseWorkerForTest(true);
            {
                LearningStore externalStore(testDbPath);
                ASSERT_TRUE(externalStore.open());
                ASSERT_TRUE(externalStore.clear());
                externalStore.close();
            }
            ASSERT_TRUE(writer.reloadIfChanged());
            ASSERT_EQ(writer.epochViewState(), EpochViewState::Reloading);
        } else {
            ASSERT_EQ(writer.epochViewState(), EpochViewState::Ready);
        }

        // 1. Chinese deviation -> Record
        CommitEvent evChiDev;
        evChiDev.text = "上海";
        evChiDev.fullPinyin = "shanghai";
        evChiDev.role = CommitCandidateRole::Chinese;
        evChiDev.candidateIndex = 1;
        evChiDev.isDeviation = true;
        evChiDev.timestampMs = 1001;
        EXPECT_TRUE(writer.enqueueCommit(evChiDev));

        // 2. Chinese core default without history ("是" / "shi") -> Skip
        CommitEvent evChiDef;
        evChiDef.text = "是";
        evChiDef.fullPinyin = "shi";
        evChiDef.role = CommitCandidateRole::Chinese;
        evChiDef.candidateIndex = 0;
        evChiDef.isDeviation = false;
        evChiDef.isPartial = false;
        evChiDef.timestampMs = 1002;
        EXPECT_TRUE(writer.enqueueCommit(evChiDef));

        // 3. English word -> Record
        CommitEvent evEng;
        evEng.text = "hello";
        evEng.fullPinyin = "hello";
        evEng.role = CommitCandidateRole::English;
        evEng.candidateIndex = 1;
        evEng.isDeviation = true;
        evEng.timestampMs = 1003;
        EXPECT_TRUE(writer.enqueueCommit(evEng));

        // 4. Raw English -> Record
        CommitEvent evRawEng;
        evRawEng.text = "code";
        evRawEng.fullPinyin = "code";
        evRawEng.role = CommitCandidateRole::Raw;
        evRawEng.candidateIndex = 1;
        evRawEng.isDeviation = true;
        evRawEng.timestampMs = 1004;
        EXPECT_TRUE(writer.enqueueCommit(evRawEng));

        // 5. Raw Pinyin sequence (not English) -> Skip
        CommitEvent evRawPin;
        evRawPin.text = "jianpan";
        evRawPin.fullPinyin = "jianpan";
        evRawPin.role = CommitCandidateRole::Raw;
        evRawPin.candidateIndex = 1;
        evRawPin.isDeviation = true;
        evRawPin.timestampMs = 1005;
        EXPECT_TRUE(writer.enqueueCommit(evRawPin));

        // 6. Command -> Skip
        CommitEvent evCmd;
        evCmd.text = "clear";
        evCmd.fullPinyin = "clear";
        evCmd.role = CommitCandidateRole::Command;
        evCmd.candidateIndex = 1;
        evCmd.isDeviation = true;
        evCmd.timestampMs = 1006;
        EXPECT_TRUE(writer.enqueueCommit(evCmd));

        if (underReloading) {
            writer.pauseWorkerForTest(false);
        }
        EXPECT_TRUE(writer.flush());
        EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);

        // Verify in-memory snapshot parity
        auto snap = writer.snapshot();
        ASSERT_NE(snap, nullptr);
        EXPECT_TRUE(snap->hasPositiveFrequency("上海", "shanghai"));
        EXPECT_FALSE(snap->hasPositiveFrequency("是", "shi"));
        EXPECT_TRUE(snap->hasPositiveFrequency("hello", "hello"));
        EXPECT_TRUE(snap->hasPositiveFrequency("code", "code"));
        EXPECT_FALSE(snap->hasPositiveFrequency("jianpan", "jianpan"));
        EXPECT_FALSE(snap->hasPositiveFrequency("clear", "clear"));

        // Verify independent database parity
        sqlite3 *db = nullptr;
        ASSERT_EQ(sqlite3_open(testDbPath.c_str(), &db), SQLITE_OK);

        auto checkDb = [&](const std::string &phrase, bool shouldExist) {
            sqlite3_stmt *stmt = nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT count(*), frequency FROM learning_entries WHERE phrase = ?;", -1, &stmt, nullptr), SQLITE_OK);
            sqlite3_bind_text(stmt, 1, phrase.c_str(), -1, SQLITE_TRANSIENT);
            ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
            if (shouldExist) {
                EXPECT_EQ(sqlite3_column_int(stmt, 0), 1) << "Phrase " << phrase << " expected in DB";
                EXPECT_EQ(sqlite3_column_int(stmt, 1), 1);
            } else {
                EXPECT_EQ(sqlite3_column_int(stmt, 0), 0) << "Phrase " << phrase << " should NOT be in DB";
            }
            sqlite3_finalize(stmt);
        };

        checkDb("上海", true);
        checkDb("是", false);
        checkDb("hello", true);
        checkDb("code", true);
        checkDb("jianpan", false);
        checkDb("clear", false);

        sqlite3_close(db);
    };

    // Test both Normal and Reloading - verify 100% parity
    runRoleMatrixTest(/*underReloading=*/false);
    runRoleMatrixTest(/*underReloading=*/true);
}

TEST_F(ArchitectureWriterRegression, ReloadingReadFailureRetainsPendingCommitsForSubsequentRecovery) {
    LearningWriter writer(dbPath());
    writer.pauseWorkerForTest(true);
    struct Resume { LearningWriter &w; ~Resume() { w.pauseWorkerForTest(false); } } resume{writer};

    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }
    ASSERT_TRUE(writer.reloadIfChanged());
    ASSERT_EQ(writer.epochViewState(), EpochViewState::Reloading);

    CommitEvent fact;
    fact.text = "上海";
    fact.fullPinyin = "shanghai";
    fact.role = CommitCandidateRole::Chinese;
    fact.candidateIndex = 1;
    fact.isDeviation = true;
    fact.timestampMs = 2000;
    writer.enqueueCommit(fact);

    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath().c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "ALTER TABLE learning_entries RENAME COLUMN suppressed TO unavailable;", nullptr, nullptr, nullptr), SQLITE_OK);

    writer.pauseWorkerForTest(false);
    EXPECT_FALSE(writer.flush());

    // CRITICAL: read failure must NOT enter Ready! Must NOT clear PendingCommit!
    EXPECT_NE(writer.epochViewState(), EpochViewState::Ready);
    EXPECT_GT(writer.queuedEventsCount(), 0u);

    ASSERT_EQ(sqlite3_exec(db, "ALTER TABLE learning_entries RENAME COLUMN unavailable TO suppressed;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);

    EXPECT_TRUE(writer.reloadIfChanged() || writer.flush());
    EXPECT_TRUE(writer.flush());
    EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("上海", "shanghai"));

    // Independent database check: verify exact frequency == 1 and count == 1
    sqlite3 *verifyDb = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath().c_str(), &verifyDb), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(verifyDb, "SELECT count(*), frequency FROM learning_entries WHERE phrase = '上海';", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    EXPECT_EQ(sqlite3_column_int(stmt, 1), 1);
    sqlite3_finalize(stmt);

    sqlite3_stmt *totalStmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(verifyDb, "SELECT count(*) FROM learning_entries;", -1, &totalStmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(totalStmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(totalStmt, 0), 1);
    sqlite3_finalize(totalStmt);
    sqlite3_close(verifyDb);
}

TEST_F(ArchitectureWriterRegression, SecondClearDuringWorkerRebaseRejectsStaleResult) {
    LearningWriter writer(dbPath());
    EXPECT_TRUE(writer.enqueueSelection("北京", "beijing", "", "", 1000));
    EXPECT_TRUE(writer.flush());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));

    // First clear
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    writer.pauseWorkerAfterReadForTest(true);
    struct ResumeAfterRead { LearningWriter &w; ~ResumeAfterRead() { w.pauseWorkerAfterReadForTest(false); } } resume{writer};

    ASSERT_TRUE(writer.reloadIfChanged());
    const auto epoch1 = writer.resetRevision();

    // Ensure worker has read epoch 1 and is waiting at the after-read synchronization barrier
    for (int i = 0; i < 500 && !writer.isWorkerWaitingAfterReadForTest(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(writer.isWorkerWaitingAfterReadForTest());

    // Now do a SECOND clear on external store
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    // Input thread observes second clear and enqueues epoch 2 commit
    ASSERT_TRUE(writer.reloadIfChanged());
    const auto epoch2 = writer.resetRevision();
    EXPECT_GT(epoch2, epoch1);

    CommitEvent fact2;
    fact2.text = "广州";
    fact2.fullPinyin = "guangzhou";
    fact2.role = CommitCandidateRole::Chinese;
    fact2.candidateIndex = 1;
    fact2.isDeviation = true;
    fact2.timestampMs = 3000;
    writer.enqueueCommit(fact2);

    writer.pauseWorkerAfterReadForTest(false);
    EXPECT_TRUE(writer.flush());

    EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);
    EXPECT_GE(writer.resetRevision(), epoch2);

    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("广州", "guangzhou"));
}

TEST_F(ArchitectureWriterRegression, PermanentReadFailureFlushesCleanlyWithoutDeadlockOrFakeReady) {
    auto tempPath = dbPath();
    {
        std::ofstream corrupt(tempPath);
        corrupt << "corrupted sqlite database content";
    }
    const auto start = std::chrono::steady_clock::now();
    {
        LearningWriter writer(tempPath);
        CommitEvent fact;
        fact.text = "测试";
        fact.fullPinyin = "ceshi";
        fact.role = CommitCandidateRole::Chinese;
        writer.enqueueCommit(fact);

        EXPECT_FALSE(writer.flush());
        EXPECT_NE(writer.epochViewState(), EpochViewState::Ready);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 1000);
}

TEST_F(ArchitectureWriterRegression, RevisionAtomicPublicationWithWorkerRebase) {
    LearningWriter writer(dbPath());
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> maxObservedRevision{0};
    std::atomic<std::size_t> readsDone{0};

    // 4 concurrent reader threads querying resetRevision(), snapshot(), and epochViewState()
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            std::int64_t lastRev = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto rev = writer.resetRevision();
                EXPECT_GE(rev, lastRev);
                lastRev = rev;

                auto snap = writer.snapshot();
                ASSERT_NE(snap, nullptr);
                const auto state = writer.epochViewState();
                (void)state;

                auto currentMax = maxObservedRevision.load(std::memory_order_relaxed);
                while (rev > currentMax &&
                       !maxObservedRevision.compare_exchange_weak(currentMax, rev, std::memory_order_relaxed)) {}

                readsDone.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
    }

    // Repeated writer rebase cycles with external clear
    for (int cycle = 0; cycle < 8; ++cycle) {
        writer.enqueueSelection("词_" + std::to_string(cycle), "ci", "", "", 1000 + cycle);
        {
            LearningStore externalStore(dbPath());
            if (externalStore.open()) {
                externalStore.clear();
                externalStore.close();
            }
        }
        writer.reloadIfChanged();
        writer.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto &r : readers) {
        r.join();
    }

    EXPECT_GT(readsDone.load(), 100u);
    EXPECT_GT(maxObservedRevision.load(), 0u);
}

TEST_F(ArchitectureWriterRegression, SecondClearDuringReloadingWithoutManualReloadPreservesNewFact) {
    auto runTest = [&](const std::filesystem::path &path) {
        LearningWriter writer(path);
        EXPECT_TRUE(writer.enqueueSelection("北京", "beijing", "", "", 1000));
        EXPECT_TRUE(writer.flush());
        EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));

        // First clear
        {
            LearningStore externalStore(path);
            ASSERT_TRUE(externalStore.open());
            ASSERT_TRUE(externalStore.clear());
            externalStore.close();
        }

        writer.pauseWorkerForTest(true);
        struct WorkerGuard {
            LearningWriter &w;
            ~WorkerGuard() {
                w.pauseWorkerForTest(false);
                w.pauseWorkerAfterReadForTest(false);
            }
        } guard{writer};

        // Reload puts writer in Reloading state
        ASSERT_TRUE(writer.reloadIfChanged());
        ASSERT_EQ(writer.epochViewState(), EpochViewState::Reloading);
        const auto epoch1 = writer.resetRevision();

        // Second clear occurs externally
        {
            LearningStore externalStore(path);
            ASSERT_TRUE(externalStore.open());
            ASSERT_TRUE(externalStore.clear());
            externalStore.close();
        }

        // Commit on input thread WITHOUT manual reload: Enter Raw English "hello"
        CommitEvent fact;
        fact.text = "hello";
        fact.fullPinyin = "hello";
        fact.role = CommitCandidateRole::Raw;
        fact.candidateIndex = 0;
        fact.isDeviation = true;
        fact.timestampMs = 3000;
        EXPECT_TRUE(writer.enqueueCommit(fact));

        // The internal refreshBeforeEnqueue should have observed epoch2 > epoch1
        EXPECT_GT(writer.resetRevision(), epoch1);

        writer.pauseWorkerForTest(false);
        EXPECT_TRUE(writer.flush());

        EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);

        // Snapshot verification: "hello" has positive frequency, "beijing" zero resurrection
        EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("北京", "beijing"));
        EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("hello", "hello"));

        // Direct database verification
        {
            LearningStore verifyStore(path);
            ASSERT_TRUE(verifyStore.openReadOnly());
            auto state = verifyStore.readState();
            ASSERT_TRUE(state.has_value());
            EXPECT_FALSE(state->snapshot->hasPositiveFrequency("北京", "beijing"));
            EXPECT_TRUE(state->snapshot->hasPositiveFrequency("hello", "hello"));
            EXPECT_EQ(state->resetRevision, writer.resetRevision());
            verifyStore.close();
        }
    };

    runTest(dbPath());
    runTest(lmdbPath());
}

TEST_F(ArchitectureWriterRegression, AsyncStartupInitialCommitWithExistingRevisionPreservesFact) {
    auto runTest = [&](const std::filesystem::path &path) {
        // Pre-populate database with non-zero revision (clear twice + baseline words)
        {
            LearningStore initStore(path);
            ASSERT_TRUE(initStore.open());
            ASSERT_TRUE(initStore.clear());
            ASSERT_TRUE(initStore.clear());
            const auto rev = *initStore.resetRevision();
            std::vector<modernime::core::LearningEvent> batch;
            for (int i = 0; i < 50; ++i) {
                modernime::core::LearningEvent e;
                e.kind = modernime::core::LearningEvent::Kind::Selection;
                e.phrase = "基线词" + std::to_string(i);
                e.pinyin = "ci";
                e.nowMs = 1000;
                e.resetRevision = rev;
                batch.push_back(std::move(e));
            }
            ASSERT_TRUE(initStore.recordBatch(batch));
            initStore.close();
        }

        LearningWriter writer(path, /*synchronousInitialLoad=*/false);
        writer.pauseWorkerAfterReadForTest(true);
        struct WorkerGuard {
            LearningWriter &w;
            ~WorkerGuard() {
                w.pauseWorkerAfterReadForTest(false);
                w.pauseWorkerForTest(false);
            }
        } guard{writer};

        // Wait until worker is waiting after read
        for (int i = 0; i < 2000 && !writer.isWorkerWaitingAfterReadForTest(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(writer.isWorkerWaitingAfterReadForTest());

        // Verify initial revision observed from disk (not 0!)
        EXPECT_GE(writer.resetRevision(), 2);

        // Input thread commits Enter Raw English "hello" while worker holds read state
        CommitEvent fact;
        fact.text = "hello";
        fact.fullPinyin = "hello";
        fact.role = CommitCandidateRole::Raw;
        fact.candidateIndex = 0;
        fact.isDeviation = true;
        fact.timestampMs = 3000;
        EXPECT_TRUE(writer.enqueueCommit(fact));

        writer.pauseWorkerAfterReadForTest(false);
        EXPECT_TRUE(writer.flush());

        EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);
        EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("hello", "hello"));

        // Direct store verification: frequency == 1, correct revision
        {
            LearningStore verifyStore(path);
            ASSERT_TRUE(verifyStore.openReadOnly());
            auto state = verifyStore.readState();
            ASSERT_TRUE(state.has_value());
            EXPECT_TRUE(state->snapshot->hasPositiveFrequency("hello", "hello"));
            EXPECT_EQ(state->resetRevision, writer.resetRevision());
            verifyStore.close();
        }
    };

    runTest(dbPath());
    runTest(lmdbPath());
}

TEST_F(ArchitectureWriterRegression, ExternalClearAfterWorkerReadRejectsStalePublication) {
    auto runTest = [&](const std::filesystem::path &path) {
        LearningWriter writer(path);
        struct WorkerGuard {
            LearningWriter &w;
            ~WorkerGuard() {
                w.pauseWorkerAfterReadForTest(false);
                w.pauseWorkerForTest(false);
            }
        } guard{writer};

        std::atomic<bool> stalePublishedAsReady{false};
        writer.setOnReadyCallback([&]() {
            if (writer.snapshot() && writer.snapshot()->hasPositiveFrequency("旧代词", "jiudaici")) {
                stalePublishedAsReady = true;
            }
        });

        writer.pauseWorkerAfterReadForTest(true);

        // Clear and record "旧代词"
        {
            LearningStore externalStore(path);
            ASSERT_TRUE(externalStore.open());
            ASSERT_TRUE(externalStore.clear());
            ASSERT_TRUE(externalStore.recordSelection("旧代词", "jiudaici", "", "", 1000));
            externalStore.close();
        }

        ASSERT_TRUE(writer.reloadIfChanged());

        for (int i = 0; i < 2000 && !writer.isWorkerWaitingAfterReadForTest(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(writer.isWorkerWaitingAfterReadForTest());

        // NOW clear externally after worker has already read!
        {
            LearningStore externalStore(path);
            ASSERT_TRUE(externalStore.open());
            ASSERT_TRUE(externalStore.clear());
            externalStore.close();
        }

        // Commit Enter Raw English "hello" without manual reload
        CommitEvent fact;
        fact.text = "hello";
        fact.fullPinyin = "hello";
        fact.role = CommitCandidateRole::Raw;
        fact.candidateIndex = 0;
        fact.isDeviation = true;
        fact.timestampMs = 3000;
        writer.enqueueCommit(fact);

        // Unpause worker: worker must detect disk revision advanced past its read state,
        // reject publishing the stale read state, and re-read.
        writer.pauseWorkerAfterReadForTest(false);
        EXPECT_TRUE(writer.flush());

        EXPECT_FALSE(stalePublishedAsReady.load());
        EXPECT_EQ(writer.epochViewState(), EpochViewState::Ready);
        EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("旧代词", "jiudaici"));
        EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("hello", "hello"));

        writer.setOnReadyCallback(nullptr);
    };

    runTest(dbPath());
    runTest(lmdbPath());
}

} // namespace

