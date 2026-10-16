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

TEST_F(ArchitectureWriterRegression, ClearThenStorageFailureBoundedQueue) {
    LearningWriter writer(dbPath());

    EXPECT_TRUE(writer.enqueueSelection("基础词", "jichuci", "", "", 1000));
    EXPECT_TRUE(writer.flush());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("基础词", "jichuci"));

    // External clear
    {
        LearningStore externalStore(dbPath());
        ASSERT_TRUE(externalStore.open());
        ASSERT_TRUE(externalStore.clear());
        externalStore.close();
    }

    // Induce deterministic storage failure by injecting an abort trigger on insert
    sqlite3 *faultInjector = nullptr;
    ASSERT_EQ(sqlite3_open(dbPath().c_str(), &faultInjector), SQLITE_OK);
    const char *trigger =
        "CREATE TRIGGER fail_storage_write BEFORE INSERT ON learning_entries "
        "BEGIN SELECT RAISE(ABORT, 'injected storage failure'); END;";
    char *err = nullptr;
    ASSERT_EQ(sqlite3_exec(faultInjector, trigger, nullptr, nullptr, &err), SQLITE_OK);
    sqlite3_free(err);

    struct FaultGuard {
        sqlite3 *db;
        ~FaultGuard() {
            if (db) {
                sqlite3_exec(db, "DROP TRIGGER IF EXISTS fail_storage_write;", nullptr, nullptr, nullptr);
                sqlite3_close(db);
            }
        }
    } faultGuard{faultInjector};

    for (int i = 0; i < 150; ++i) {
        writer.enqueueSelection("词" + std::to_string(i), "ci", "", "", 2000 + i);
    }

    // Flush will attempt to persist, fail due to trigger, and return false
    EXPECT_FALSE(writer.flush());
    EXPECT_LE(writer.queuedEventsCount(), 100u);
    EXPECT_GT(writer.queuedEventsCount(), 0u);
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

} // namespace
