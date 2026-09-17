#include "modernime/core/candidate_ranker.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"

#include <sqlite3.h>

#include <cstdlib>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "learning store test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testPath(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-" + std::string(name));
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void testSelectionPersistsAcrossReopen() {
    const auto path = testPath("learning.sqlite3");
    modernime::core::LearningStore first(path);
    assertTrue(first.open(), "learning store opens");
    assertTrue(first.recordSelection("你好", "ni'hao", "今", "，", 1000),
               "selection is stored");
    first.close();

    modernime::core::LearningStore second(path);
    assertTrue(second.open(), "learning store reopens");
    const auto snapshot = second.snapshot(1000);
    assertTrue(snapshot != nullptr, "snapshot is available");
    assertTrue(snapshot->boostAt("你好", "nihao", 1000) > 0.0,
               "persisted selection boosts the candidate");
    second.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testFrequencyBonusIsBoundedAndMovesCandidate() {
    modernime::core::CandidateScore native{0, "普通词", "changyongci", 0.0F};
    modernime::core::CandidateScore learned{1, "常用词", "changyongci", 0.0F};
    learned.learning_boost = 2.0;
    std::vector candidates{native, learned};
    const auto order = modernime::core::CandidateRanker::rank(
        "changyongci", candidates);
    assertTrue(order.front() == 1, "learned candidate moves ahead");
    assertTrue(learned.learning_boost <= 4.0,
               "learning bonus remains bounded");
}

void testPreviousOrderAddsStabilityForNearTies() {
    modernime::core::CandidateScore first{0, "甲", "a", 0.0F};
    modernime::core::CandidateScore second{1, "乙", "a", 0.0F};
    second.learning_boost = 0.15;
    std::vector candidates{first, second};
    const std::vector<std::string> previousOrder{
        modernime::core::candidateOrderKey("乙", "a"),
        modernime::core::candidateOrderKey("甲", "a")};
    modernime::core::CandidateRanker::rank("a", candidates, previousOrder);
    assertTrue(candidates[1].stability_bonus > 0.0,
               "near-tied previous candidate receives stability bonus");
}

void testCandidateOrderKeyIsSharedByRankingLayers() {
    assertTrue(modernime::core::candidateOrderKey("候选", "hou'xuan") ==
                   std::string("候选") + '\x1f' + "hou'xuan",
               "candidate order key uses text before pinyin");
}

void testDecoderScoreIsNormalizedAsASecondarySignal() {
    modernime::core::CandidateScore weaker{0, "甲", "a", 4.0F};
    modernime::core::CandidateScore stronger{1, "乙", "a", 1.0F};
    std::vector candidates{weaker, stronger};
    modernime::core::CandidateRanker::rank("a", candidates);
    assertTrue(candidates[1].decoder_bonus > candidates[0].decoder_bonus,
               "lower decoder score receives a larger normalized bonus");
    assertTrue(candidates[1].final_score() <
                   static_cast<double>(candidates[1].source_index),
               "decoder bonus participates in the final score");
}

void testNegativeFeedbackReducesLearningBoost() {
    const auto path = testPath("negative-learning.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "negative feedback store opens");
    assertTrue(store.recordSelection("错误词", "cuowuci", "", "", 1000),
               "selection is stored before negative feedback");
    assertTrue(store.recordNegativeFeedback("错误词", "cuowuci"),
               "negative feedback is stored");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->boostAt("错误词", "cuowuci", 1000) < 1.0,
               "negative feedback reduces the boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testNegativeFeedbackWithoutSelectionDoesNotCreatePositiveBoost() {
    const auto path = testPath("negative-only-learning.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "negative-only store opens");
    assertTrue(store.recordNegativeFeedback("从未选过", "congweixuanguo"),
               "negative-only feedback is stored");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->boostAt("从未选过", "congweixuanguo", 1000) <=
                   0.0,
               "negative-only feedback never creates a positive learning boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testSuppressionPersistsAndDisablesLearningBoost() {
    const auto path = testPath("suppressed-learning.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "suppression store opens");
    assertTrue(store.recordSelection("被删词", "beishanci", {}, {}, 1000),
               "selection is stored before suppression");
    assertTrue(store.recordSuppression("被删词", "beishanci"),
               "suppression is stored");
    const auto snapshot = store.snapshot(1000);
    const auto *entry = snapshot->entry("被删词", "beishanci", {}, {});
    assertTrue(entry != nullptr && entry->suppressed,
               "suppression survives in the snapshot");
    assertTrue(snapshot->isSuppressed("被删词", "beishanci"),
               "suppression is visible by candidate key");
    assertTrue(snapshot->boostAt("被删词", "beishanci", 1000) == 0.0,
               "suppressed candidates receive no learning boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testCorruptedLearningCountersRemainFinite() {
    const std::vector<modernime::core::LearningEntry> entries{
        {"损坏频次", "sunhuaici", {}, {}, -3, -1000, 0},
        {"损坏负反馈", "sunfankuici", {}, {}, 0, 0, -3}};
    const modernime::core::LearningSnapshot snapshot(entries);
    const auto frequencyBoost =
        snapshot.boostAt("损坏频次", "sunhuaici", 1000);
    const auto feedbackBoost =
        snapshot.boostAt("损坏负反馈", "sunfankuici", 1000);
    assertTrue(std::isfinite(frequencyBoost) && frequencyBoost == 0.0 &&
                   std::isfinite(feedbackBoost) && feedbackBoost == 0.0,
               "negative persisted counters are ignored safely");
}

void testLearningCountersSaturateAtMaximum() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const std::vector<modernime::core::LearningEntry> entries{
        {"饱和词", "baheci", {}, {}, maximum, 1000, maximum}};
    modernime::core::LearningSnapshot snapshot(entries);
    snapshot.recordSelection("饱和词", "baheci", {}, {}, 2000);
    snapshot.recordNegativeFeedback("饱和词", "baheci");
    const auto &stored = snapshot.entries().front();
    assertTrue(stored.frequency == maximum && stored.negativeFeedback == maximum,
               "learning counters saturate instead of overflowing");
}

void testCorruptedSelectionTimestampDoesNotOverflow() {
    const std::vector<modernime::core::LearningEntry> entries{
        {"损坏时间", "sunhuaici", {}, {}, 1,
         std::numeric_limits<std::int64_t>::min(), 0}};
    const modernime::core::LearningSnapshot snapshot(entries);
    const auto boost = snapshot.boostAt(
        "损坏时间", "sunhuaici", std::numeric_limits<std::int64_t>::max());
    assertTrue(std::isfinite(boost) && boost < 1.0,
               "corrupted selection timestamps are treated as old safely");
}

void testPersistedLearningCountersSaturateAtMaximum() {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto path = testPath("persisted-counter-overflow.sqlite3");
    sqlite3 *database = nullptr;
    assertTrue(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
               "persistent overflow store can be created");
    const auto schema =
        "CREATE TABLE learning_entries ("
        "pinyin TEXT NOT NULL, phrase TEXT NOT NULL, "
        "context_before TEXT NOT NULL DEFAULT '', "
        "context_after TEXT NOT NULL DEFAULT '', "
        "frequency INTEGER NOT NULL DEFAULT 0, "
        "last_selected_ms INTEGER NOT NULL DEFAULT 0, "
        "negative_feedback INTEGER NOT NULL DEFAULT 0, "
        "PRIMARY KEY (pinyin, phrase, context_before, context_after));";
    char *error = nullptr;
    assertTrue(sqlite3_exec(database, schema, nullptr, nullptr, &error) ==
                   SQLITE_OK,
               "persistent overflow schema is created");
    sqlite3_free(error);
    const auto insert =
        "INSERT INTO learning_entries (pinyin, phrase, frequency, "
        "negative_feedback) VALUES ('baheci', '饱和词', " +
        std::to_string(maximum) + ", " + std::to_string(maximum) + ");";
    assertTrue(sqlite3_exec(database, insert.c_str(), nullptr, nullptr,
                            &error) == SQLITE_OK,
               "maximum counters are inserted");
    sqlite3_free(error);
    sqlite3_close(database);

    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "persistent overflow store opens");
    assertTrue(store.recordSelection("饱和词", "baheci", {}, {}, 2000),
               "selection at maximum counter succeeds");
    assertTrue(store.recordNegativeFeedback("饱和词", "baheci"),
               "feedback at maximum counter succeeds");
    const auto snapshot = store.snapshot();
    const auto *entry = snapshot->entry("饱和词", "baheci", {}, {});
    assertTrue(entry != nullptr && entry->frequency == maximum &&
                   entry->negativeFeedback == maximum,
               "persistent counters saturate instead of overflowing");
    store.close();
    std::error_code errorCode;
    std::filesystem::remove(path, errorCode);
    std::filesystem::remove(path.string() + "-wal", errorCode);
    std::filesystem::remove(path.string() + "-shm", errorCode);
}

void testMatchingContextRaisesCandidate() {
    const auto path = testPath("context.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "context store opens");
    assertTrue(store.recordSelection("你好", "nihao", "今天天气", "很好", 1000),
               "context selection stores");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->contextBoost("你好", "nihao", "今天天气", "很好") >
                   0.0,
               "matching context adds a bounded boost");
    assertTrue(snapshot->contextBoost("你好", "nihao", "完全不同", "") == 0.0,
               "unmatched context adds no boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testNearbyContextWindowStillMatches() {
    const auto path = testPath("nearby-context.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "nearby context store opens");
    assertTrue(store.recordSelection("窗口词", "chuangkouci", "甲乙丙丁",
                                     "戊己庚辛", 1000),
               "nearby context selection stores");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->contextBoost("窗口词", "chuangkouci", "新甲乙丙丁",
                                      "戊己庚辛新") > 0.0,
               "a shifted context window still contributes a bounded boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testContextHistoryRemainsBoundedPerCandidate() {
    const auto path = testPath("bounded-context.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "bounded context store opens");
    for (int index = 0; index < 16; ++index) {
        assertTrue(store.recordSelection(
                       "上下文限额词", "shangxiawenxianeci",
                       "前文" + std::to_string(index), "后文", 1000 + index),
                   "context variant is stored");
    }
    const auto snapshot = store.snapshot();
    assertTrue(snapshot->entries().size() <= 8,
               "context history is bounded per candidate");
    assertTrue(snapshot->entry("上下文限额词", "shangxiawenxianeci", "前文15",
                               "后文") != nullptr,
               "the most recent context variant is retained");
    store.close();
    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "bounded context store reopens");
    const auto reopenedSnapshot = reopened.snapshot();
    assertTrue(reopenedSnapshot->entries().size() <= 8,
               "bounded context history remains bounded after reopening");
    reopened.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testBaseNegativeFeedbackAppliesToContextualSelection() {
    const auto path = testPath("context-negative-learning.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "context negative-feedback store opens");
    assertTrue(store.recordSelection("上下文词", "shangxiawen", "前文",
                                     "后文", 1000),
               "contextual selection stores");
    assertTrue(store.recordNegativeFeedback("上下文词", "shangxiawen"),
               "base negative feedback stores");
    const auto snapshot = store.snapshot(1000);
    assertTrue(snapshot->boostAt("上下文词", "shangxiawen", 1000) < 1.0,
               "base negative feedback lowers contextual selection boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testWriterReportsUnavailableStoreAndKeepsMemorySnapshot() {
    const auto path = std::filesystem::path("/dev/null") /
                      "modernime-learning.sqlite3";
    modernime::core::LearningWriter writer(path);
    assertTrue(!writer.enqueueSelection("内存词", "neicun" , {}, {}, 1000),
               "writer rejects durable event when store cannot open");
    const auto snapshot = writer.snapshot();
    assertTrue(snapshot->boostAt("内存词", "neicun", 1000) > 0.0,
               "in-memory learning remains available after storage failure");
    assertTrue(!writer.flush(), "flush reports unavailable storage");
}

void testWriterRejectsMalformedStoreAtStartup() {
    const auto path = testPath("malformed-learning.sqlite3");
    sqlite3 *database = nullptr;
    assertTrue(sqlite3_open(path.c_str(), &database) == SQLITE_OK,
               "malformed store can be created for the test");
    char *error = nullptr;
    assertTrue(sqlite3_exec(database,
                            "CREATE TABLE learning_entries (broken INTEGER);",
                            nullptr, nullptr, &error) == SQLITE_OK,
               "malformed learning schema is created");
    sqlite3_free(error);
    sqlite3_close(database);

    modernime::core::LearningWriter writer(path);
    assertTrue(!writer.enqueueSelection("内存词", "neicun", {}, {}, 1000),
               "malformed store is rejected before durable enqueue");
    assertTrue(writer.snapshot()->boostAt("内存词", "neicun", 1000) >
                   0.0,
               "memory learning remains available after schema failure");
    assertTrue(!writer.flush(), "flush reports malformed storage");

    std::error_code errorCode;
    std::filesystem::remove(path, errorCode);
    std::filesystem::remove(path.string() + "-wal", errorCode);
    std::filesystem::remove(path.string() + "-shm", errorCode);
}

void testWriterFlushesSelectionBeforeReopen() {
    const auto path = testPath("writer.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        assertTrue(writer.enqueueSelection("写入词", "xieruci", {}, {}, 1000),
                   "writer accepts selection for a valid store");
        assertTrue(writer.enqueueSelection("写入词", "xieruci", {}, {}, 2000),
                   "writer accepts repeated selection for a valid store");
        assertTrue(writer.enqueueNegativeFeedback("写入词", "xieruci"),
                   "writer accepts feedback for a valid store");
        assertTrue(writer.flush(), "flush confirms durable learning write");
    }
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "flushed writer store reopens");
    const auto snapshot = store.snapshot();
    const auto *entry = snapshot->entry("写入词", "xieruci", {}, {});
    assertTrue(entry != nullptr && entry->frequency == 2 &&
                   entry->negativeFeedback == 1,
               "flushed batch survives writer shutdown");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testWriterRetainsFailedBatchAndRecoversAfterStoreUnlocks() {
    const auto path = testPath("writer-retry.sqlite3");
    {
        modernime::core::LearningWriter writer(path);

        sqlite3 *locker = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &locker) == SQLITE_OK,
                   "retry test opens a competing database connection");
        char *error = nullptr;
        assertTrue(sqlite3_exec(locker, "BEGIN IMMEDIATE;", nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "retry test holds the database write lock");
        sqlite3_free(error);

        assertTrue(writer.enqueueSelection("重试词", "chongshici", {}, {},
                                           1000),
                   "writer accepts an event before the temporary failure");
        assertTrue(!writer.flush(),
                   "flush reports failure after a bounded retry cycle");

        error = nullptr;
        assertTrue(sqlite3_exec(locker, "ROLLBACK;", nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "retry test releases the database write lock");
        sqlite3_free(error);
        sqlite3_close(locker);

        bool recoveredWithoutAnotherEnqueue = false;
        const auto recoveryDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < recoveryDeadline) {
            modernime::core::LearningStore observer(path);
            if (observer.open()) {
                const auto observed = observer.snapshot(1000);
                const auto *entry =
                    observed->entry("重试词", "chongshici", {}, {});
                recoveredWithoutAnotherEnqueue =
                    entry != nullptr && entry->frequency == 1;
                observer.close();
            }
            if (recoveredWithoutAnotherEnqueue) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assertTrue(recoveredWithoutAnotherEnqueue,
                   "retained events resume automatically after the store recovers");
        assertTrue(writer.flush(),
                   "flush reports success after autonomous recovery");
    }

    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "recovered writer store reopens");
    const auto snapshot = reopened.snapshot(1000);
    const auto *entry = snapshot->entry("重试词", "chongshici", {}, {});
    assertTrue(entry != nullptr && entry->frequency == 1,
               "the failed batch is recovered exactly once");
    reopened.close();

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testWriterAutomaticallyRecoversFromTemporaryWriteLock() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-auto-retry.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        sqlite3 *locker = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &locker) == SQLITE_OK,
                   "automatic retry test opens a competing connection");
        char *error = nullptr;
        assertTrue(sqlite3_exec(locker, "BEGIN IMMEDIATE;", nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "automatic retry test holds the database write lock");
        sqlite3_free(error);

        assertTrue(writer.enqueueSelection("自动恢复词", "zidonghuifu", {},
                                           {}, 1000),
                   "writer accepts an event before a temporary lock");
        auto flushResult = std::async(std::launch::async,
                                      [&writer] { return writer.flush(); });

        std::this_thread::sleep_for(1500ms);
        error = nullptr;
        assertTrue(sqlite3_exec(locker, "ROLLBACK;", nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "automatic retry test releases the temporary lock");
        sqlite3_free(error);
        sqlite3_close(locker);

        assertTrue(flushResult.wait_for(4s) == std::future_status::ready,
                   "flush completes after the temporary fault clears");
        assertTrue(flushResult.get(),
                   "flush succeeds through the bounded automatic retry cycle");
    }

    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "automatically recovered store reopens");
    const auto snapshot = reopened.snapshot(1000);
    const auto *entry = snapshot->entry("自动恢复词", "zidonghuifu", {}, {});
    assertTrue(entry != nullptr && entry->frequency == 1,
               "automatic retry persists the event exactly once");
    reopened.close();

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testWriterQueuesSelectionsArrivingDuringRecovery() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-recovery-window.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        sqlite3 *faultInjector = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &faultInjector) == SQLITE_OK,
                   "recovery-window test opens its fault injector");
        char *error = nullptr;
        const auto trigger =
            "CREATE TRIGGER fail_writer_insert BEFORE INSERT ON "
            "learning_entries BEGIN SELECT RAISE(ABORT, 'temporary writer "
            "failure'); END;";
        assertTrue(sqlite3_exec(faultInjector, trigger, nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "recovery-window test injects a write failure");
        sqlite3_free(error);

        assertTrue(writer.enqueueSelection("故障前词", "guzhangqian", {}, {},
                                           1000),
                   "writer accepts the selection that encounters the fault");
        assertTrue(!writer.flush(),
                   "flush reports the first bounded failure cycle");
        std::this_thread::sleep_for(500ms);

        assertTrue(!writer.enqueueSelection("恢复中词", "huifuzhong", {}, {},
                                            2000),
                   "enqueue reports that durability is not yet restored");

        error = nullptr;
        assertTrue(sqlite3_exec(faultInjector,
                                "DROP TRIGGER fail_writer_insert;", nullptr,
                                nullptr, &error) == SQLITE_OK,
                   "recovery-window test clears the write failure");
        sqlite3_free(error);
        sqlite3_close(faultInjector);

        assertTrue(writer.flush(),
                   "flush persists retained and recovery-window selections");
    }

    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "recovery-window store reopens");
    const auto snapshot = reopened.snapshot(2000);
    assertTrue(snapshot->entry("故障前词", "guzhangqian", {}, {}) != nullptr,
               "the selection that hit the fault is retained");
    assertTrue(snapshot->entry("恢复中词", "huifuzhong", {}, {}) != nullptr,
               "a selection arriving during recovery is persisted");
    reopened.close();

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testWriterDestructorRetriesRetainedBatchAfterFaultClears() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-destructor-recovery.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        sqlite3 *faultInjector = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &faultInjector) == SQLITE_OK,
                   "destructor recovery test opens its fault injector");
        char *error = nullptr;
        const auto trigger =
            "CREATE TRIGGER fail_destructor_insert BEFORE INSERT ON "
            "learning_entries BEGIN SELECT RAISE(ABORT, 'temporary writer "
            "failure'); END;";
        assertTrue(sqlite3_exec(faultInjector, trigger, nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "destructor recovery test injects a write failure");
        sqlite3_free(error);

        assertTrue(writer.enqueueSelection("析构恢复词", "xigouhuifu", {}, {},
                                           1000),
                   "writer accepts the selection before destructor recovery");
        assertTrue(!writer.flush(),
                   "destructor recovery test observes a bounded failure");
        std::this_thread::sleep_for(500ms);

        error = nullptr;
        assertTrue(sqlite3_exec(faultInjector,
                                "DROP TRIGGER fail_destructor_insert;",
                                nullptr, nullptr, &error) == SQLITE_OK,
                   "destructor recovery test clears the write failure");
        sqlite3_free(error);
        sqlite3_close(faultInjector);
    }

    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "destructor-recovered store reopens");
    assertTrue(reopened.snapshot(1000)->entry("析构恢复词", "xigouhuifu",
                                              {}, {}) != nullptr,
               "destructor performs a bounded final write of the retained batch");
    reopened.close();

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testWriterDestructorReturnsUnderPermanentFailure() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-destructor-permanent-failure.sqlite3");
    sqlite3 *faultInjector = nullptr;
    const auto started = std::chrono::steady_clock::now();
    {
        modernime::core::LearningWriter writer(path);
        assertTrue(sqlite3_open(path.c_str(), &faultInjector) == SQLITE_OK,
                   "permanent-failure test opens its fault injector");
        char *error = nullptr;
        const auto trigger =
            "CREATE TRIGGER fail_permanent_insert BEFORE INSERT ON "
            "learning_entries BEGIN SELECT RAISE(ABORT, 'permanent writer "
            "failure'); END;";
        assertTrue(sqlite3_exec(faultInjector, trigger, nullptr, nullptr,
                                &error) == SQLITE_OK,
                   "permanent-failure test injects a persistent write failure");
        sqlite3_free(error);
        assertTrue(writer.enqueueSelection("永久故障词", "yongjiuguzhang", {},
                                           {}, 1000),
                   "writer accepts the selection before permanent failure");
        assertTrue(!writer.flush(),
                   "permanent-failure flush completes with failure");
        std::this_thread::sleep_for(500ms);
    }
    assertTrue(std::chrono::steady_clock::now() - started < 3s,
               "destructor remains bounded under a permanent storage failure");

    char *error = nullptr;
    assertTrue(sqlite3_exec(faultInjector,
                            "DROP TRIGGER fail_permanent_insert;", nullptr,
                            nullptr, &error) == SQLITE_OK,
               "permanent-failure test clears its injected trigger");
    sqlite3_free(error);
    sqlite3_close(faultInjector);

    std::error_code errorCode;
    std::filesystem::remove(path, errorCode);
    std::filesystem::remove(path.string() + "-wal", errorCode);
    std::filesystem::remove(path.string() + "-shm", errorCode);
}

void testLearningBatchPersistsAsOneLogicalUpdate() {
    const auto path = testPath("learning-batch.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "batch learning store opens");
    const std::vector<modernime::core::LearningEvent> events{
        {modernime::core::LearningEvent::Kind::Selection, "批量词",
         "piliangci", {}, {}, 1000},
        {modernime::core::LearningEvent::Kind::Selection, "批量词",
         "piliangci", {}, {}, 2000},
        {modernime::core::LearningEvent::Kind::NegativeFeedback, "批量词",
         "piliangci", {}, {}, 0}};
    assertTrue(store.recordBatch(events), "learning events commit as a batch");
    const auto snapshot = store.snapshot(2000);
    const auto *entry = snapshot->entry("批量词", "piliangci", {}, {});
    assertTrue(entry != nullptr && entry->frequency == 2 &&
                   entry->negativeFeedback == 1,
               "batch preserves every learning event");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testFailedBatchCanBeRetriedWithoutDuplicatingCommittedSelections() {
    const auto path = testPath("learning-batch-rollback.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "retry-safe batch store opens");

    sqlite3 *faultInjector = nullptr;
    assertTrue(sqlite3_open(path.c_str(), &faultInjector) == SQLITE_OK,
               "batch retry test opens its fault-injection connection");
    char *error = nullptr;
    const auto trigger =
        "CREATE TRIGGER fail_learning_prune BEFORE DELETE ON "
        "learning_entries BEGIN SELECT RAISE(ABORT, 'injected prune "
        "failure'); END;";
    assertTrue(sqlite3_exec(faultInjector, trigger, nullptr, nullptr, &error) ==
                   SQLITE_OK,
               "batch retry test injects a deterministic prune failure");
    sqlite3_free(error);

    std::vector<modernime::core::LearningEvent> events;
    for (int index = 0; index < 9; ++index) {
        events.push_back({modernime::core::LearningEvent::Kind::Selection,
                          "事务重试词", "shiwuchongshi",
                          "前文" + std::to_string(index), "后文", 1000 + index});
    }
    assertTrue(!store.recordBatch(events),
               "injected housekeeping failure fails the whole batch");

    error = nullptr;
    assertTrue(sqlite3_exec(faultInjector, "DROP TRIGGER fail_learning_prune;",
                            nullptr, nullptr, &error) == SQLITE_OK,
               "batch retry test clears the injected failure");
    sqlite3_free(error);
    sqlite3_close(faultInjector);

    assertTrue(store.recordBatch(events), "the same batch succeeds on retry");
    const auto snapshot = store.snapshot();
    assertTrue(snapshot->entries().size() == 8,
               "context pruning still applies to the retried batch");
    for (const auto &entry : snapshot->entries()) {
        assertTrue(entry.frequency == 1,
                   "a retried batch never duplicates a committed selection");
    }
    store.close();

    std::error_code errorCode;
    std::filesystem::remove(path, errorCode);
    std::filesystem::remove(path.string() + "-wal", errorCode);
    std::filesystem::remove(path.string() + "-shm", errorCode);
}

void testTotalEntryLimitEvictsSuppressedThenOldest() {
    const auto path = testPath("capped-learning.sqlite3");
    modernime::core::LearningStore store(path);
    store.setTotalEntryLimit(4);
    assertTrue(store.open(), "capped store opens");
    assertTrue(store.recordSelection("旧甲", "jijia", {}, {}, 1000),
               "first old entry stores");
    assertTrue(store.recordSelection("旧乙", "jiyi", {}, {}, 1100),
               "second old entry stores");
    assertTrue(store.recordSuppression("旧乙", "jiyi"),
               "second old entry is suppressed");
    assertTrue(store.recordSelection("新甲", "xinjia", {}, {}, 3000),
               "first recent entry stores");
    assertTrue(store.recordSelection("新乙", "xinyi", {}, {}, 3100),
               "second recent entry stores");
    assertTrue(store.snapshot()->entries().size() == 4,
               "the store stays at the total entry limit");

    assertTrue(store.recordSelection("新丙", "xinbing", {}, {}, 3200),
               "third recent entry stores");
    const auto afterSuppressedEviction = store.snapshot();
    assertTrue(afterSuppressedEviction->entries().size() == 4,
               "the limit holds when over capacity");
    assertTrue(afterSuppressedEviction->entry("旧乙", "jiyi", {}, {}) == nullptr,
               "the suppressed entry is evicted first");
    assertTrue(afterSuppressedEviction->entry("旧甲", "jijia", {}, {}) != nullptr,
               "the older unsuppressed entry survives the suppressed one");

    assertTrue(store.recordSelection("新丁", "xinding", {}, {}, 3300),
               "fourth recent entry stores");
    const auto afterOldestEviction = store.snapshot();
    assertTrue(afterOldestEviction->entries().size() == 4,
               "the limit still holds");
    assertTrue(afterOldestEviction->entry("旧甲", "jijia", {}, {}) == nullptr,
               "the oldest unsuppressed entry is evicted next");
    assertTrue(afterOldestEviction->entry("新甲", "xinjia", {}, {}) != nullptr &&
                   afterOldestEviction->entry("新乙", "xinyi", {}, {}) != nullptr &&
                   afterOldestEviction->entry("新丙", "xinbing", {}, {}) != nullptr &&
                   afterOldestEviction->entry("新丁", "xinding", {}, {}) != nullptr,
               "recent entries are retained");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testSnapshotAppliesTotalEntryLimit() {
    std::vector<modernime::core::LearningEntry> entries;
    for (int index = 0; index < 6; ++index) {
        entries.push_back({"词" + std::to_string(index), "ci", {}, {}, 1,
                           1000 + index, 0});
    }
    const modernime::core::LearningSnapshot snapshot(entries, 3);
    assertTrue(snapshot.entries().size() == 3,
               "the snapshot constructor applies the total limit");
    assertTrue(snapshot.entry("词5", "ci", {}, {}) != nullptr,
               "the most recent entries are retained");
    assertTrue(snapshot.entry("词0", "ci", {}, {}) == nullptr,
               "the oldest entries are evicted");

    std::vector<modernime::core::LearningEntry> seed{
        {"甲", "jia", {}, {}, 1, 1000, 0},
        {"乙", "yi", {}, {}, 1, 2000, 0}};
    modernime::core::LearningSnapshot mutating(seed, 2);
    mutating.recordSelection("丙", "bing", {}, {}, 5000);
    assertTrue(mutating.entries().size() == 2,
               "mutations respect the total limit");
    assertTrue(mutating.entry("丙", "bing", {}, {}) != nullptr,
               "the newly selected entry is retained");
    assertTrue(mutating.entry("甲", "jia", {}, {}) == nullptr,
               "the oldest entry is evicted by the mutation");
}

void testFrequencyAccumulatesAcrossContextVariants() {
    const auto path = testPath("cross-context-learning.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "cross-context store opens");
    // The same word selected once in each of several different contexts:
    // each selection lands in its own context variant row.
    assertTrue(store.recordSelection("高频词", "gaopinci", "前甲", "后甲", 1000),
               "first context selection stores");
    assertTrue(store.recordSelection("高频词", "gaopinci", "前乙", "后乙", 2000),
               "second context selection stores");
    assertTrue(store.recordSelection("高频词", "gaopinci", "前丙", "后丙", 3000),
               "third context selection stores");
    const auto snapshot = store.snapshot(3000);
    assertTrue(snapshot->boostAt("高频词", "gaopinci", 3000) > 1.5,
               "aggregated frequency boosts a brand-new context");
    assertTrue(snapshot->hasPositiveFrequency("高频词", "gaopinci"),
               "aggregated frequency classifies the candidate as learned");
    assertTrue(snapshot->boostAt("从未选过", "congweixuanguo", 3000) == 0.0,
               "a word without selections receives no boost");
    assertTrue(!snapshot->hasPositiveFrequency("从未选过", "congweixuanguo"),
               "a word without selections is not classified as learned");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testHighFrequencyBoostExceedsTheOldCeiling() {
    const std::vector<modernime::core::LearningEntry> entries{
        {"常用词", "changyongci", {}, {}, 100, 1000, 0}};
    const modernime::core::LearningSnapshot snapshot(entries);
    const auto boost = snapshot.boostAt("常用词", "changyongci", 1000);
    assertTrue(boost > 3.5,
               "high accumulated frequency pushes the boost past the old ceiling");
    assertTrue(boost <= 4.0, "the learning boost stays bounded");
}

void testHighFrequencyDeepCandidateReachesTheTop() {
    std::vector<modernime::core::CandidateScore> candidates;
    for (std::size_t index = 0; index < 60; ++index) {
        candidates.push_back({index, "词" + std::to_string(index), "zd", 0.0F});
    }
    const auto initialOrder =
        modernime::core::CandidateRanker::rank("zd", candidates);
    assertTrue(initialOrder[53] == 53, "initially candidate 53 is at rank 53");

    candidates[53].learning_boost = 2.94;
    const auto learnedOrder =
        modernime::core::CandidateRanker::rank("zd", candidates);
    assertTrue(learnedOrder.front() == 53,
               "frequently selected deep candidate rises to the very top (rank 0)");
}

void testDeeplyLearnedCandidateReachesTheFront() {
    std::vector<modernime::core::CandidateScore> candidates;
    for (std::size_t index = 0; index < 13; ++index) {
        candidates.push_back({index, "词" + std::to_string(index), "a", 0.0F});
    }
    candidates.back().learning_boost = 3.9;
    const auto order = modernime::core::CandidateRanker::rank("a", candidates);
    assertTrue(order.front() == candidates.size() - 1,
               "a strongly learned tail candidate reaches the front");
}

} // namespace

int main() {
    testSelectionPersistsAcrossReopen();
    testFrequencyBonusIsBoundedAndMovesCandidate();
    testPreviousOrderAddsStabilityForNearTies();
    testCandidateOrderKeyIsSharedByRankingLayers();
    testDecoderScoreIsNormalizedAsASecondarySignal();
    testNegativeFeedbackReducesLearningBoost();
    testNegativeFeedbackWithoutSelectionDoesNotCreatePositiveBoost();
    testSuppressionPersistsAndDisablesLearningBoost();
    testCorruptedLearningCountersRemainFinite();
    testLearningCountersSaturateAtMaximum();
    testCorruptedSelectionTimestampDoesNotOverflow();
    testPersistedLearningCountersSaturateAtMaximum();
    testMatchingContextRaisesCandidate();
    testNearbyContextWindowStillMatches();
    testContextHistoryRemainsBoundedPerCandidate();
    testBaseNegativeFeedbackAppliesToContextualSelection();
    testWriterReportsUnavailableStoreAndKeepsMemorySnapshot();
    testWriterRejectsMalformedStoreAtStartup();
    testWriterFlushesSelectionBeforeReopen();
    testWriterRetainsFailedBatchAndRecoversAfterStoreUnlocks();
    testWriterAutomaticallyRecoversFromTemporaryWriteLock();
    testWriterQueuesSelectionsArrivingDuringRecovery();
    testWriterDestructorRetriesRetainedBatchAfterFaultClears();
    testWriterDestructorReturnsUnderPermanentFailure();
    testLearningBatchPersistsAsOneLogicalUpdate();
    testFailedBatchCanBeRetriedWithoutDuplicatingCommittedSelections();
    testTotalEntryLimitEvictsSuppressedThenOldest();
    testSnapshotAppliesTotalEntryLimit();
    testFrequencyAccumulatesAcrossContextVariants();
    testHighFrequencyBoostExceedsTheOldCeiling();
    testHighFrequencyDeepCandidateReachesTheTop();
    testDeeplyLearnedCandidateReachesTheFront();
    return EXIT_SUCCESS;
}
