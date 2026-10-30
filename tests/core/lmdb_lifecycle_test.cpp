#include "modernime/core/learning_store.h"
#include "modernime/core/personal_engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using modernime::core::LearningEvent;
using modernime::core::LearningStore;
using modernime::core::PersonalEngine;

class ArchitectureLmdbRegression : public testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-lmdb-lifecycle-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::filesystem::path path() const {
        return root_ / "test_lifecycle.lmdb";
    }

    std::filesystem::path root_;
};

LearningEvent makeSelection(std::string phrase, std::string pinyin, uint32_t nowMs = 1'000'000) {
    LearningEvent event;
    event.phrase = std::move(phrase);
    event.pinyin = std::move(pinyin);
    event.nowMs = nowMs;
    return event;
}

// 1. OpenWhileBatchExits:
// Tests in a separate child process that concurrent OpenLmdb/CloseLmdb and RecordLearningBatch
// (including success, oversized rejection, map-full resize, and capacity staging exit)
// never deadlock due to lock inversions between registry and environment handles.
// The child process must exit cleanly within 3.0 seconds, otherwise the test fails.
TEST_F(ArchitectureLmdbRegression, OpenWhileBatchExits) {
    const auto dbPath = path();

    const pid_t childPid = ::fork();
    ASSERT_GE(childPid, 0);

    if (childPid == 0) {
        // Child process
        std::atomic<bool> done{false};

        // Thread 1: Concurrently opens, queries, and closes LMDB
        std::thread openCloseThread([&]() {
            for (int i = 0; i < 40 && !done.load(std::memory_order_relaxed); ++i) {
                PersonalEngine engine;
                if (engine.OpenLmdb(dbPath)) {
                    (void)engine.QueryLmdbCandidates("test");
                    (void)engine.QueryLmdbPrefixCandidates("te");
                    engine.CloseLmdb();
                }
                std::this_thread::yield();
            }
        });

        // Thread 2: Concurrently executes batches covering:
        // - Success batch
        // - Oversized rejection batch (phrase > 512)
        // - Map expansion
        // - Staged state exit
        std::thread batchThread([&]() {
            PersonalEngine writer;
            if (!writer.OpenLmdb(dbPath)) {
                return;
            }

            for (int i = 0; i < 30; ++i) {
                // 1) Success batch
                std::vector<LearningEvent> successBatch{
                    makeSelection("词语" + std::to_string(i), "ciyu" + std::to_string(i), 1000 + i)
                };
                (void)writer.RecordLearningBatch(successBatch);

                // 2) Oversized rejection batch (triggers early rejection and staged state destruction)
                std::vector<LearningEvent> oversizedBatch{
                    makeSelection(std::string(600, 'a'), "oversized", 2000 + i)
                };
                (void)writer.RecordLearningBatch(oversizedBatch);

                // 3) Map expansion / multi-item batch
                std::vector<LearningEvent> multiBatch;
                for (int j = 0; j < 10; ++j) {
                    multiBatch.push_back(makeSelection(
                        "多词" + std::to_string(i) + "_" + std::to_string(j),
                        "duoci" + std::to_string(j),
                        3000 + i * 10 + j));
                }
                (void)writer.RecordLearningBatch(multiBatch);

                std::this_thread::yield();
            }

            writer.CloseLmdb();
            done.store(true, std::memory_order_release);
        });

        batchThread.join();
        openCloseThread.join();

        std::_Exit(0);
    }

    // Parent process: Watchdog timer of 3.0 seconds
    int waitStatus = 0;
    bool childExitedFast = false;
    for (int i = 0; i < 120; ++i) { // 120 * 25ms = 3.0s
        const pid_t waited = ::waitpid(childPid, &waitStatus, WNOHANG);
        if (waited == childPid) {
            childExitedFast = WIFEXITED(waitStatus) && (WEXITSTATUS(waitStatus) == 0);
            break;
        }
        ::usleep(25000);
    }

    if (!childExitedFast) {
        ::kill(childPid, SIGKILL);
        ::waitpid(childPid, &waitStatus, 0);
    }

    EXPECT_TRUE(childExitedFast) << "Child process timed out or crashed; potential deadlock or lock inversion";
}

// 2. OpenResizeCloseInterleaving:
// Multiple engines operating on the same path with concurrent queries, writes causing map resizes,
// and open/close cycles. Ensures no deadlocks, no dangling mmap pointers, and data consistency.
TEST_F(ArchitectureLmdbRegression, OpenResizeCloseInterleaving) {
    const auto dbPath = path();

    PersonalEngine primary;
    ASSERT_TRUE(primary.OpenLmdb(dbPath, /*read_only=*/false));

    std::atomic<bool> stopFlag{false};

    // Thread 1: Query thread
    std::thread readerThread([&]() {
        PersonalEngine reader;
        if (!reader.OpenLmdb(dbPath, /*read_only=*/true)) {
            return;
        }
        while (!stopFlag.load(std::memory_order_relaxed)) {
            auto results = reader.QueryLmdbCandidates("pinyin");
            auto prefixes = reader.QueryLmdbPrefixCandidates("pin");
            std::this_thread::yield();
        }
        reader.CloseLmdb();
    });

    // Thread 2: Transient lifecycle thread (rapidly open/close)
    std::thread transientThread([&]() {
        for (int i = 0; i < 25 && !stopFlag.load(std::memory_order_relaxed); ++i) {
            PersonalEngine transient;
            if (transient.OpenLmdb(dbPath, /*read_only=*/false)) {
                (void)transient.RecordUsage("pin", "拼");
                (void)transient.QueryLmdbCandidates("pin");
                transient.CloseLmdb();
            }
            std::this_thread::yield();
        }
    });

    // Main thread: Insert records to force map growth / writes
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(primary.RecordUsage("pinyin", "拼音" + std::to_string(i), 1.0f, "", {}, 10000 + i));
    }

    stopFlag.store(true, std::memory_order_release);
    transientThread.join();
    readerThread.join();

    EXPECT_GE(primary.QueryLmdbCandidates("pinyin").size(), 1u);
    primary.CloseLmdb();
}

// 3. ReadOnlyUpgradeSharesEnvironment:
// An environment initially opened as read-only by one engine is upgraded to writable
// by a subsequent engine, sharing the single underlying environment without dangling DBI/env.
TEST_F(ArchitectureLmdbRegression, ReadOnlyUpgradeSharesEnvironment) {
    const auto dbPath = path();

    // First create a populated database so read_only open succeeds even before write
    {
        PersonalEngine initEngine;
        ASSERT_TRUE(initEngine.OpenLmdb(dbPath, /*read_only=*/false));
        ASSERT_TRUE(initEngine.RecordUsage("init", "初始"));
        initEngine.CloseLmdb();
    }

    // Engine 1 opens read-only
    PersonalEngine engine1;
    ASSERT_TRUE(engine1.OpenLmdb(dbPath, /*read_only=*/true));
    EXPECT_TRUE(engine1.IsLmdbOpen());
    auto initCandidates = engine1.QueryLmdbCandidates("init");
    ASSERT_EQ(initCandidates.size(), 1u);
    EXPECT_EQ(initCandidates[0].text, "初始");

    // Engine 2 opens read-write on the same path -> triggers upgradeToWritable
    PersonalEngine engine2;
    ASSERT_TRUE(engine2.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_TRUE(engine2.IsLmdbOpen());

    // Engine 2 writes new record
    ASSERT_TRUE(engine2.RecordUsage("upgrade", "升级"));

    // Engine 1 queries the new record from the shared upgraded environment
    auto upgradedCandidates = engine1.QueryLmdbCandidates("upgrade");
    ASSERT_FALSE(upgradedCandidates.empty());
    EXPECT_EQ(upgradedCandidates[0].text, "升级");

    // Detach engine 1; engine 2 must remain fully functional
    engine1.CloseLmdb();
    EXPECT_FALSE(engine1.IsLmdbOpen());

    ASSERT_TRUE(engine2.RecordUsage("upgrade2", "升级二"));
    auto secondCandidates = engine2.QueryLmdbCandidates("upgrade2");
    ASSERT_FALSE(secondCandidates.empty());
    EXPECT_EQ(secondCandidates[0].text, "升级二");

    engine2.CloseLmdb();
    EXPECT_FALSE(engine2.IsLmdbOpen());
}

// 4. LastOwnerClosesOnce:
// Detaching the first engine leaves the second operational; closing the last owner unregisters
// cleanly from LmdbRegistry, and a subsequent open gets a clean new environment.
TEST_F(ArchitectureLmdbRegression, LastOwnerClosesOnce) {
    const auto dbPath = path();

    PersonalEngine engine1;
    PersonalEngine engine2;

    ASSERT_TRUE(engine1.OpenLmdb(dbPath, /*read_only=*/false));
    ASSERT_TRUE(engine2.OpenLmdb(dbPath, /*read_only=*/false));

    ASSERT_TRUE(engine1.RecordUsage("word1", "词语一"));

    // Detach first owner
    engine1.CloseLmdb();
    EXPECT_FALSE(engine1.IsLmdbOpen());

    // Second owner must still be fully functional
    auto res1 = engine2.QueryLmdbCandidates("word1");
    ASSERT_FALSE(res1.empty());
    EXPECT_EQ(res1[0].text, "词语一");

    ASSERT_TRUE(engine2.RecordUsage("word2", "词语二"));
    auto res2 = engine2.QueryLmdbCandidates("word2");
    ASSERT_FALSE(res2.empty());
    EXPECT_EQ(res2[0].text, "词语二");

    // Close second (last) owner -> unregisters from registry
    engine2.CloseLmdb();
    EXPECT_FALSE(engine2.IsLmdbOpen());

    // A third engine opens the database cleanly
    PersonalEngine engine3;
    ASSERT_TRUE(engine3.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_TRUE(engine3.IsLmdbOpen());

    auto check1 = engine3.QueryLmdbCandidates("word1");
    ASSERT_FALSE(check1.empty());
    EXPECT_EQ(check1[0].text, "词语一");

    auto check2 = engine3.QueryLmdbCandidates("word2");
    ASSERT_FALSE(check2.empty());
    EXPECT_EQ(check2[0].text, "词语二");

    engine3.CloseLmdb();
    EXPECT_FALSE(engine3.IsLmdbOpen());
}

// 5. ConcurrentRoAndRwInitialization (T11-A11):
// Multiple threads concurrently open the same LMDB database where some threads request
// read-only and others request read-write. Neither crashes, deadlocks, nor receives an
// invalid read-only environment when writable was requested.
TEST_F(ArchitectureLmdbRegression, ConcurrentRoAndRwInitialization) {
    const auto dbPath = path();
    {
        PersonalEngine seed;
        ASSERT_TRUE(seed.OpenLmdb(dbPath, /*read_only=*/false));
        ASSERT_TRUE(seed.RecordUsage("seed", "种子"));
        seed.CloseLmdb();
    }

    constexpr int kIterations = 15;
    for (int iter = 0; iter < kIterations; ++iter) {
        std::atomic<bool> start{false};
        std::atomic<bool> ro_ok{false};
        std::atomic<bool> rw_ok{false};
        const std::string word = "word_" + std::to_string(iter);

        std::thread t_ro([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            PersonalEngine roEngine;
            if (roEngine.OpenLmdb(dbPath, /*read_only=*/true)) {
                auto cands = roEngine.QueryLmdbCandidates("seed");
                ro_ok.store(!cands.empty(), std::memory_order_release);
                roEngine.CloseLmdb();
            }
        });

        std::thread t_rw([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            PersonalEngine rwEngine;
            if (rwEngine.OpenLmdb(dbPath, /*read_only=*/false)) {
                bool wrote = rwEngine.RecordUsage("test", word);
                rw_ok.store(wrote, std::memory_order_release);
                rwEngine.CloseLmdb();
            }
        });

        start.store(true, std::memory_order_release);
        t_ro.join();
        t_rw.join();

        EXPECT_TRUE(ro_ok.load(std::memory_order_acquire));
        EXPECT_TRUE(rw_ok.load(std::memory_order_acquire));
    }
}

// 6. MapFullTriggersRealMapResize (T11-A12):
// Verifies that when inserting enough data to exceed the initial 64 KiB LMDB map size,
// the batch write catches MDB_MAP_FULL, dynamically resizes the map (at least doubling it),
// and completes all writes without data loss, deadlocks, or dangling pointers, even while
// concurrent readers and transient open/close sessions are actively running.
TEST_F(ArchitectureLmdbRegression, MapFullTriggersRealMapResize) {
    const auto dbPath = path();

    modernime::core::EngineConfig cfg;
    cfg.initial_lmdb_map_size = 64 * 1024;
    PersonalEngine engine(cfg);
    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/false));
    const size_t initialMapSize = engine.LmdbMapSize();
    EXPECT_EQ(initialMapSize, 65536u);

    // Insert 600 unique records with context to exceed initial 64 KiB
    std::vector<LearningEvent> events;
    events.reserve(600);
    for (int i = 0; i < 600; ++i) {
        LearningEvent event;
        event.kind = LearningEvent::Kind::Selection;
        event.phrase = "词语_" + std::to_string(i) + "_具有长上下文长文本内容用于填充页面空间";
        event.pinyin = "ciyu_" + std::to_string(i);
        event.contextBefore = "前置上下文_" + std::to_string(i);
        event.contextAfter = "后置上下文_" + std::to_string(i);
        event.nowMs = 1000000 + i;
        events.push_back(std::move(event));
    }

    std::atomic<bool> stopConcurrent{false};
    std::atomic<std::size_t> readsCompleted{0};

    // Concurrent reader thread: repeatedly queries the shared LMDB database while resize occurs
    std::thread readerThread([&]() {
        PersonalEngine reader;
        if (reader.OpenLmdb(dbPath, /*read_only=*/true)) {
            while (!stopConcurrent.load(std::memory_order_relaxed)) {
                auto res = reader.QueryLmdbCandidates("ciyu_0");
                auto pre = reader.QueryLmdbPrefixCandidates("ciyu_");
                readsCompleted.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
            reader.CloseLmdb();
        }
    });

    // Concurrent transient thread: rapidly opens and closes secondary engine on same dbPath
    std::thread transientThread([&]() {
        for (int i = 0; i < 15 && !stopConcurrent.load(std::memory_order_relaxed); ++i) {
            PersonalEngine transient;
            if (transient.OpenLmdb(dbPath, /*read_only=*/false)) {
                transient.QueryLmdbCandidates("ciyu_0");
                transient.CloseLmdb();
            }
            std::this_thread::yield();
        }
    });

    ASSERT_TRUE(engine.RecordLearningBatch(events));

    stopConcurrent.store(true, std::memory_order_relaxed);
    readerThread.join();
    transientThread.join();

    const size_t newMapSize = engine.LmdbMapSize();
    EXPECT_GT(newMapSize, initialMapSize);
    EXPECT_GE(newMapSize, 131072u);

    // Verify candidates are retrievable
    auto queryResult = engine.QueryLmdbCandidates("ciyu_100");
    ASSERT_FALSE(queryResult.empty());
    EXPECT_EQ(queryResult[0].text, "词语_100_具有长上下文长文本内容用于填充页面空间");

    engine.CloseLmdb();
}

// 7. SwitchDatabasePreservesLifecycleWithoutStaleCachedTxn:
// Switching database paths after caching a read transaction must not retain or abort a stale
// transaction handle on an already-closed MDB_env (preventing SIGSEGV on switch/close).
TEST_F(ArchitectureLmdbRegression, SwitchDatabasePreservesLifecycleWithoutStaleCachedTxn) {
    const auto pathA = root_ / "db_switch_a.lmdb";
    const auto pathB = root_ / "db_switch_b.lmdb";

    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(pathA, /*read_only=*/false));
    EXPECT_EQ(engine.ResetRevision(), 0);

    // Switch to path B; path A environment closes cleanly without leaking or dangling cached txn
    ASSERT_TRUE(engine.OpenLmdb(pathB, /*read_only=*/false));
    EXPECT_EQ(engine.ResetRevision(), 0);

    // Close engine cleanly
    engine.CloseLmdb();
    EXPECT_FALSE(engine.IsLmdbOpen());
}

// 8. ReadOnlyUpgradeInvalidatesCachedRevisionTxn:
// Upgrading a read-only environment to writable invalidates any cached revision transaction,
// and subsequent revision queries see newly committed revisions without stale transaction aborts.
TEST_F(ArchitectureLmdbRegression, ReadOnlyUpgradeInvalidatesCachedRevisionTxn) {
    const auto dbPath = root_ / "db_upgrade_rev.lmdb";

    // Seed database with revision 0
    {
        PersonalEngine seed;
        ASSERT_TRUE(seed.OpenLmdb(dbPath, /*read_only=*/false));
        ASSERT_TRUE(seed.RecordUsage("init", "初始"));
        seed.CloseLmdb();
    }

    // Engine 1 opens read-only and populates its cached revision transaction
    PersonalEngine engine1;
    ASSERT_TRUE(engine1.OpenLmdb(dbPath, /*read_only=*/true));
    EXPECT_EQ(engine1.ResetRevision(), 0);

    // Engine 2 opens read-write on the same path, upgrading the environment
    PersonalEngine engine2;
    ASSERT_TRUE(engine2.OpenLmdb(dbPath, /*read_only=*/false));
    ASSERT_TRUE(engine2.Clear());
    EXPECT_EQ(engine2.ResetRevision(), 1);

    // Engine 1 queries revision again; it must observe revision 1 through a renewed transaction on the upgraded env
    EXPECT_EQ(engine1.ResetRevision(), 1);

    engine1.CloseLmdb();
    engine2.CloseLmdb();
}

// 9. CloseAndReopenSamePathSafelyRecreatesCachedRevisionTxn:
// Closing and reopening the same database path safely cleans up and recreates the cached revision transaction.
TEST_F(ArchitectureLmdbRegression, CloseAndReopenSamePathSafelyRecreatesCachedRevisionTxn) {
    const auto dbPath = root_ / "db_reopen_rev.lmdb";

    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_EQ(engine.ResetRevision(), 0);
    engine.CloseLmdb();

    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/false));
    ASSERT_TRUE(engine.Clear());
    EXPECT_EQ(engine.ResetRevision(), 1);
    engine.CloseLmdb();
}

// 10. ConcurrentRevisionReadsAndCloseInterleaving:
// Multiple threads concurrently reading ResetRevision while an engine clears and closes.
TEST_F(ArchitectureLmdbRegression, ConcurrentRevisionReadsAndCloseInterleaving) {
    const auto dbPath = root_ / "db_concurrent_rev.lmdb";

    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/false));

    std::atomic<bool> stopFlag{false};
    std::atomic<int> readSuccessCount{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            PersonalEngine reader;
            if (reader.OpenLmdb(dbPath, /*read_only=*/true)) {
                while (!stopFlag.load(std::memory_order_relaxed)) {
                    int64_t rev = reader.ResetRevision();
                    EXPECT_GE(rev, 0);
                    readSuccessCount.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
                reader.CloseLmdb();
            }
        });
    }

    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_TRUE(engine.Clear());
    }

    stopFlag.store(true, std::memory_order_relaxed);
    for (auto &t : readers) {
        t.join();
    }

    EXPECT_GT(readSuccessCount.load(), 0);
    EXPECT_EQ(engine.ResetRevision(), 5);
    engine.CloseLmdb();
}

// 11. Test_A16_SameInstanceReadOnlyToReadWriteUpgrade:
// Verifies that calling OpenLmdb(path, false) on an already RO-opened engine
// properly upgrades the instance to RW mode so subsequent writes succeed.
TEST_F(ArchitectureLmdbRegression, SameInstanceReadOnlyToReadWriteUpgrade) {
    const auto dbPath = root_ / "db_ro_rw_upgrade.lmdb";

    {
        PersonalEngine seed;
        ASSERT_TRUE(seed.OpenLmdb(dbPath, /*read_only=*/false));
        ASSERT_TRUE(seed.RecordUsage("init", "初始"));
    }

    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/true));
    EXPECT_FALSE(engine.RecordUsage("forbidden", "禁写"));

    ASSERT_TRUE(engine.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_TRUE(engine.RecordUsage("allowed", "写入成功"));

    PersonalEngine verifier;
    ASSERT_TRUE(verifier.OpenLmdb(dbPath, /*read_only=*/true));
    EXPECT_EQ(verifier.EntryCount().value_or(0), 2);
}

// 12. Test_A15_StaleFlushRejectedAfterExternalClear:
// Verifies that an engine with a stale baseline loaded before Clear() cannot
// FlushToLmdb(), preventing revision rollback and resurrection of cleared entries.
TEST_F(ArchitectureLmdbRegression, StaleFlushRejectedAfterExternalClear) {
    const auto dbPath = root_ / "db_stale_flush.lmdb";

    {
        PersonalEngine seed;
        ASSERT_TRUE(seed.OpenLmdb(dbPath, /*read_only=*/false));
        ASSERT_TRUE(seed.RecordUsage("old", "旧词"));
    }

    PersonalEngine oldWriter;
    ASSERT_TRUE(oldWriter.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_EQ(oldWriter.ResetRevision(), 0);

    PersonalEngine clearer;
    ASSERT_TRUE(clearer.OpenLmdb(dbPath, /*read_only=*/false));
    EXPECT_EQ(clearer.ResetRevision(), 0);

    ASSERT_TRUE(clearer.Clear());
    EXPECT_EQ(clearer.ResetRevision(), 1);
    EXPECT_EQ(clearer.EntryCount().value_or(999), 0);

    // Stale flush from old writer MUST be rejected
    EXPECT_FALSE(oldWriter.FlushToLmdb());

    // Verified facts on disk: revision remains 1 and count remains 0
    EXPECT_EQ(clearer.ResetRevision(), 1);
    EXPECT_EQ(clearer.EntryCount().value_or(999), 0);

    PersonalEngine freshVerifier;
    ASSERT_TRUE(freshVerifier.OpenLmdb(dbPath, /*read_only=*/true));
    EXPECT_EQ(freshVerifier.ResetRevision(), 1);
    EXPECT_EQ(freshVerifier.EntryCount().value_or(999), 0);
}

} // namespace
