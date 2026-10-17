#include "modernime/core/candidate_ranker.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/personal_engine.h"
#include "modernime/core/phrase_formation.h"

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

void testExternalClearDiscardsRetainedDeltas() {
    const auto path = testPath("external-clear.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        assertTrue(writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000) && writer.flush(),
                   "active writer persists a learned entry");
        modernime::core::LearningStore settings(path);
        assertTrue(settings.open(), "settings connection stays open in WAL mode");
        const auto before = std::filesystem::last_write_time(path);
        sqlite3 *fault = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &fault) == SQLITE_OK,
                   "fault injection connection opens");
        assertTrue(sqlite3_exec(fault,
            "CREATE TRIGGER fail_clear_queue BEFORE INSERT ON learning_entries "
            "BEGIN SELECT RAISE(FAIL, 'temporary failure'); END;",
            nullptr, nullptr, nullptr) == SQLITE_OK, "queued write fails deterministically");
        assertTrue(writer.enqueueSelection("待写旧词", "daixiejiuci", {}, {}, 1001),
                   "selection is queued before settings clear");
        assertTrue(!writer.flush(), "old selection remains queued after storage failure");
        const auto backup = path.string() + ".backup";
        assertTrue(settings.backupTo(backup) && settings.clear(),
                   "settings backup and clear completes while engine remains active");
        assertTrue(std::filesystem::last_write_time(path) == before,
                   "WAL clear does not change the main database mtime");
        assertTrue(sqlite3_exec(fault, "DROP TRIGGER fail_clear_queue;",
                               nullptr, nullptr, nullptr) == SQLITE_OK,
                   "queued writes can resume after clear");
        sqlite3_close(fault);
        assertTrue(writer.reload(), "active writer reload completes after clear");
        assertTrue(writer.snapshot()->entries().empty() && settings.snapshot()->entries().empty(),
                   "pre-clear deltas cannot resurrect removed entries in memory or storage");
        assertTrue(writer.enqueueSelection("新词", "xinci", {}, {}, 1002) && writer.flush(),
                   "new learning works after clear");
        assertTrue(settings.snapshot()->entries().size() == 1 &&
                       settings.snapshot()->entry("新词", "xinci", {}, {}) != nullptr,
                   "only new learning is persisted after clear");
        modernime::core::LearningStore saved(backup);
        assertTrue(saved.open() && saved.snapshot()->entry("旧词", "jiuci", {}, {}) != nullptr,
                   "the backup preserves the old learned entry");
        saved.close();
        std::filesystem::remove(backup);
    }
    std::filesystem::remove(path);
}

void testWalClearResumesNewWritesAfterRetryExhaustion() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-clear-resume.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        modernime::core::LearningStore settings(path);
        assertTrue(settings.open(), "clear-resume settings store opens");
        sqlite3 *fault = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &fault) == SQLITE_OK &&
                       sqlite3_exec(fault,
                           "CREATE TRIGGER fail_resume BEFORE INSERT ON learning_entries "
                           "BEGIN SELECT RAISE(FAIL, 'temporary failure'); END;",
                           nullptr, nullptr, nullptr) == SQLITE_OK,
                   "clear-resume test injects a temporary write failure");
        assertTrue(writer.enqueueSelection("旧事件", "jiushijian", {}, {}, 1000),
                   "a pre-clear event is queued");
        assertTrue(!writer.flush(), "failed event is retained for retries");
        // Allow the writer's one automatic 250ms retry cycle to exhaust.
        std::this_thread::sleep_for(400ms);
        assertTrue(settings.clear(), "settings clears the failed queue generation");
        assertTrue(sqlite3_exec(fault, "DROP TRIGGER fail_resume;", nullptr,
                               nullptr, nullptr) == SQLITE_OK,
                   "new generation writes can succeed");
        sqlite3_close(fault);
        assertTrue(writer.reloadIfChanged(), "timer applies the WAL clear");
        assertTrue(writer.enqueueSelection("新事件", "xinshijian", {}, {}, 1001),
                   "new event is accepted after timer refresh");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool persisted = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (settings.snapshot()->entry("新事件", "xinshijian", {}, {}) != nullptr) {
                persisted = true;
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        assertTrue(persisted && settings.snapshot()->entries().size() == 1,
                   "new writes resume automatically without flush and old deltas stay deleted");
    }
    std::filesystem::remove(path);
}

void testFailedWriterPollDoesNotReopenWritableConnection() {
    using namespace std::chrono_literals;
    const auto path = testPath("writer-readonly-poll.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        sqlite3 *fault = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &fault) == SQLITE_OK &&
                       sqlite3_exec(fault,
                           "CREATE TRIGGER fail_poll BEFORE INSERT ON learning_entries "
                           "BEGIN SELECT RAISE(FAIL, 'temporary failure'); END;",
                           nullptr, nullptr, nullptr) == SQLITE_OK,
                   "poll test injects a persistent write failure");
        writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000);
        assertTrue(!writer.flush(), "writer closes its failed writable connection");
        std::this_thread::sleep_for(600ms);
        assertTrue(sqlite3_exec(fault, "BEGIN IMMEDIATE;", nullptr, nullptr,
                               nullptr) == SQLITE_OK,
                   "external transaction holds a write lock after retry exhaustion");
        auto polling = std::async(std::launch::async,
                                   [&writer] { return writer.reloadIfChanged(); });
        const auto status = polling.wait_for(200ms);
        assertTrue(sqlite3_exec(fault, "ROLLBACK; DROP TRIGGER fail_poll;",
                               nullptr, nullptr, nullptr) == SQLITE_OK,
                   "poll test releases the competing writer");
        polling.get();
        sqlite3_close(fault);
        assertTrue(status == std::future_status::ready,
                   "input-loop polling never reopens the failed writable SQLite connection");
    }
    std::filesystem::remove(path);
}

void testFirstSelectionAfterUnobservedClearPersists() {
    const auto path = testPath("writer-first-post-clear.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        modernime::core::LearningStore settings(path);
        assertTrue(settings.open() &&
                       writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000) && writer.flush(),
                   "old selection is durable before external clear");
        assertTrue(settings.clear(), "settings clear returns before engine polling");
        assertTrue(writer.enqueueSelection("新词", "xinci", {}, {}, 1001) && writer.flush(),
                   "the first post-clear selection is accepted before the timer tick");
        const auto disk = settings.snapshot();
        const auto *entry = disk->entry("新词", "xinci", {}, {});
        assertTrue(entry != nullptr && entry->frequency == 1 &&
                       disk->entries().size() == 1 && writer.snapshot()->entries().size() == 1,
                   "post-clear learning persists without reviving old cache entries");
    }
    std::filesystem::remove(path);
}

void testWriterDetectsWalClearAndSerializesOwnWrites() {
    const auto path = testPath("writer-wal-reset.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        modernime::core::LearningStore settings(path);
        assertTrue(settings.open(), "second WAL connection opens");
        assertTrue(writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000) && writer.flush(),
                   "old learning persists before clear");
        const auto oldSnapshot = writer.snapshot();
        const auto before = std::filesystem::last_write_time(path);
        assertTrue(settings.clear(), "settings clears learning through its production API");
        assertTrue(std::filesystem::last_write_time(path) == before,
                   "WAL clear leaves main database mtime unchanged");
        assertTrue(writer.reloadIfChanged() && writer.snapshot()->entries().empty(),
                   "production change detector observes WAL-only clear and resets ranking cache");
        assertTrue(!writer.reloadIfChanged(), "unchanged reset marker does not reload twice");
        assertTrue(oldSnapshot->entry("旧词", "jiuci", {}, {}) != nullptr,
                   "published snapshots remain immutable across external clear");
        auto selections = std::async(std::launch::async, [&writer] {
            for (int index = 0; index < 300; ++index) {
                assertTrue(writer.enqueueSelection("新词", "xinci", {}, {}, 2000 + index),
                           "async writer accepts selections during change polling");
            }
        });
        for (int index = 0; index < 100; ++index) {
            assertTrue(!writer.reloadIfChanged(),
                       "writer's own updates do not masquerade as external resets");
        }
        selections.get();
        assertTrue(writer.flush(), "concurrent poll and writes finish");
        const auto memory = writer.snapshot();
        const auto disk = settings.snapshot();
        const auto *cached = memory->entry("新词", "xinci", {}, {});
        const auto *stored = disk->entry("新词", "xinci", {}, {});
        assertTrue(cached != nullptr && stored != nullptr &&
                       cached->frequency == 300 && stored->frequency == 300 &&
                       memory->entries().size() == 1 && disk->entries().size() == 1,
                   "concurrent own writes preserve all deltas without restoring cleared rows");
        const bool cleared = settings.clear();
        bool reloaded = false;
        for (int i = 0; i < 20 && !reloaded; ++i) {
            reloaded = writer.reloadIfChanged();
            if (!reloaded) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        assertTrue(cleared && reloaded && writer.snapshot()->entries().empty(),
                   "a second WAL reset is detected within the same engine lifetime");
    }
    std::filesystem::remove(path);
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
        auto polling = std::async(std::launch::async,
                                   [&writer] { return writer.reloadIfChanged(); });
        assertTrue(polling.wait_for(200ms) == std::future_status::ready,
                   "settings polling does not block the input loop on a busy learning writer");
        assertTrue(!polling.get(), "busy polling retries at the next timer tick");
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

void testDeltaOverlayCopyAndQueryPerformance() {
    std::vector<modernime::core::LearningEntry> base;
    base.reserve(2000);
    for (int i = 0; i < 2000; ++i) {
        base.push_back({"短语" + std::to_string(i), "pinyin" + std::to_string(i),
                        "前文", "后文", 5, 1000, 0, false});
    }
    modernime::core::LearningSnapshot snapshot(std::move(base));
    assertTrue(snapshot.entries().size() == 2000, "snapshot initialized with 2000 entries");
    assertTrue(snapshot.hasPositiveFrequency("短语0", "pinyin0"), "initial query succeeds");

    const auto start = std::chrono::steady_clock::now();
    auto current = snapshot;
    for (int i = 0; i < 1000; ++i) {
        auto next = current;
        next.recordSelection("短语" + std::to_string(i % 2000),
                             "pinyin" + std::to_string(i % 2000),
                             "前文", "后文", 2000 + i);
        current = std::move(next);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    assertTrue(elapsed.count() < 1000, "1000 snapshot copy+mutations finish rapidly in Debug");

    const auto *entry = current.entry("短语0", "pinyin0", "前文", "后文");
    assertTrue(entry != nullptr, "updated entry exists");
    assertTrue(entry->frequency > 5, "updated entry frequency reflects selections");
}

void testCommitEventTrackingAndLearning() {
    const auto path = testPath("commit-event-tracking.sqlite3");
    {
        modernime::core::LearningStore store(path);
        assertTrue(store.open(), "store opens for commit event tracking");

        modernime::core::CommitEvent chineseEvent;
        chineseEvent.text = "王绍龙";
        chineseEvent.fullPinyin = "wang'shao'long";
        chineseEvent.rawKeystrokes = "wangshaolong";
        chineseEvent.role = modernime::core::CommitCandidateRole::Chinese;
        chineseEvent.contextBefore = "测试";
        chineseEvent.contextAfter = "结尾";
        chineseEvent.timestampMs = 12345;
        assertTrue(store.recordCommit(chineseEvent), "recordCommit succeeds for Chinese candidate");

        modernime::core::CommitEvent commandEvent;
        commandEvent.text = "剪贴板";
        commandEvent.rawKeystrokes = "v2";
        commandEvent.role = modernime::core::CommitCandidateRole::Command;
        commandEvent.timestampMs = 12346;
        assertTrue(store.recordCommit(commandEvent), "recordCommit succeeds (ignored) for Command");

        auto snapshot = store.snapshot();
        assertTrue(snapshot->entry("王绍龙", "wang'shao'long", "测试", "结尾") != nullptr,
                   "Chinese commit event is persisted in learning snapshot");
        assertTrue(snapshot->entry("剪贴板", "剪贴板", "", "") == nullptr,
                   "Command candidate is not recorded into learning store");

        modernime::core::LearningWriter writer(path);
        modernime::core::CommitEvent rawEvent;
        rawEvent.text = "gnueabihf";
        rawEvent.fullPinyin = "gnueabihf";
        rawEvent.rawKeystrokes = "gnueabihf";
        rawEvent.role = modernime::core::CommitCandidateRole::Raw;
        rawEvent.timestampMs = 12347;
        assertTrue(writer.enqueueCommit(rawEvent), "enqueueCommit succeeds for Raw candidate");
        assertTrue(writer.enqueueCommit(commandEvent), "enqueueCommit succeeds for Command candidate");
        assertTrue(writer.flush(), "writer flush succeeds");

        assertTrue(writer.snapshot()->entry("gnueabihf", "gnueabihf", "", "") != nullptr,
                   "Raw candidate commit is recorded and flushed to snapshot");
        assertTrue(writer.snapshot()->entry("剪贴板", "v2", "", "") == nullptr,
                   "Command candidate is ignored by writer enqueueCommit");
        store.close();
    }
    std::filesystem::remove(path);
}

void testIndependentRecallIndex() {
    std::vector<modernime::core::LearningEntry> entries{
        {"王绍龙", "wangshaolong", "", "", 5, 1000, 0, false},
        {"gnueabihf", "gnueabihf", "", "", 3, 1000, 0, false},
        {"不良词", "bu'liang'ci", "", "", 5, 1000, 0, true},
        {"未选词", "wei'xuan'ci", "", "", 0, 1000, 0, false},
    };
    modernime::core::LearningSnapshot snapshot(entries);

    // 1. Canonical full pinyin match (even when stored without apostrophes)
    auto recalledFull = snapshot.recallCandidates("wangshaolong", 1000);
    assertTrue(recalledFull.size() == 1, "exact canonical recall returns 1 candidate");
    assertTrue(recalledFull[0].phrase == "王绍龙", "recalled phrase matches");
    assertTrue(recalledFull[0].pinyin == "wang'shao'long", "recalled pinyin is segmented with apostrophes");
    assertTrue(recalledFull[0].learningBoost > 2.0, "recalled candidate has positive boost");

    // 2. Abbreviation match for unseparated SQLite entry
    auto recalledAbbr = snapshot.recallCandidates("wsl", 1000);
    assertTrue(recalledAbbr.size() == 1, "abbreviation recall returns 1 candidate");
    assertTrue(recalledAbbr[0].phrase == "王绍龙", "recalled phrase matches via abbreviation");
    assertTrue(recalledAbbr[0].pinyin == "wang'shao'long", "recalled abbreviation pinyin is segmented");

    // 3. Prefix match (e.g. English term)
    auto recalledPrefix = snapshot.recallCandidates("gnu", 1000);
    assertTrue(!recalledPrefix.empty(), "prefix gnu recalls gnueabihf");
    assertTrue(recalledPrefix[0].phrase == "gnueabihf", "prefix recalled phrase matches");

    auto recalledPrefix2 = snapshot.recallCandidates("gnue", 1000);
    assertTrue(!recalledPrefix2.empty() && recalledPrefix2[0].phrase == "gnueabihf",
               "prefix gnue recalls gnueabihf");

    // 4. Suppressed candidate must NOT be recalled
    auto recalledSuppressed = snapshot.recallCandidates("buliangci", 1000);
    assertTrue(recalledSuppressed.empty(), "suppressed entry is not recalled");

    // 5. Zero frequency candidate must NOT be recalled
    auto recalledZero = snapshot.recallCandidates("weixuanci", 1000);
    assertTrue(recalledZero.empty(), "zero frequency entry is not recalled");

    // 6. Dynamic delta recall
    snapshot.recordSelection("深空测控", "shen'kong'ce'kong", "", "", 2000);
    auto recalledDelta = snapshot.recallCandidates("shenkongcekong", 2000);
    assertTrue(!recalledDelta.empty(), "delta entry is recalled by canonical pinyin");
    assertTrue(recalledDelta[0].phrase == "深空测控", "delta recalled phrase matches");

    auto recalledDeltaAbbr = snapshot.recallCandidates("skck", 2000);
    assertTrue(!recalledDeltaAbbr.empty(), "delta entry is recalled by abbreviation");
    assertTrue(recalledDeltaAbbr[0].phrase == "深空测控", "delta abbreviation recalled phrase matches");
}

void testEnglishWordRecallIndex() {
    std::vector<modernime::core::LearningEntry> entries{
        {"gnueabihf", "gnueabihf", "", "", 5, 1000, 0, false},
        {"Kubernetes", "kubernetes", "", "", 3, 1000, 0, false},
        {"gnupg", "gnupg", "", "", 1, 1000, 0, false},
        {"suppressed_term", "suppressed_term", "", "", 5, 1000, 0, true},
        {"zero_term", "zero_term", "", "", 0, 1000, 0, false},
        {"王绍龙", "wang'shao'long", "", "", 5, 1000, 0, false},
    };
    modernime::core::LearningSnapshot snapshot(entries);

    // 1. Prefix matches for gnu, gnue, gnuea
    auto gnuMatches = snapshot.recallEnglishWords("gnu", 5, 1000);
    assertTrue(gnuMatches.size() == 2, "gnu prefix recalls 2 candidates");
    assertTrue(gnuMatches[0].text == "gnueabihf", "gnueabihf ranks first for gnu due to frequency");
    assertTrue(gnuMatches[1].text == "gnupg", "gnupg ranks second for gnu");

    auto gnueMatches = snapshot.recallEnglishWords("gnue", 5, 1000);
    assertTrue(gnueMatches.size() == 1, "gnue prefix recalls 1 candidate");
    assertTrue(gnueMatches[0].text == "gnueabihf", "gnue prefix recalls gnueabihf");

    auto gnueaMatches = snapshot.recallEnglishWords("gnuea", 5, 1000);
    assertTrue(gnueaMatches.size() == 1, "gnuea prefix recalls 1 candidate");
    assertTrue(gnueaMatches[0].text == "gnueabihf", "gnuea prefix recalls gnueabihf");

    // 2. Case-insensitive search with casing preservation
    auto kubeMatches = snapshot.recallEnglishWords("kube", 5, 1000);
    assertTrue(kubeMatches.size() == 1, "kube prefix recalls Kubernetes");
    assertTrue(kubeMatches[0].text == "Kubernetes", "casing is preserved as Kubernetes");

    // 3. Suppressed, zero frequency, and Chinese phrases are excluded
    auto suppMatches = snapshot.recallEnglishWords("supp", 5, 1000);
    assertTrue(suppMatches.empty(), "suppressed word is not recalled");

    auto zeroMatches = snapshot.recallEnglishWords("zero", 5, 1000);
    assertTrue(zeroMatches.empty(), "zero frequency word is not recalled");

    auto wangMatches = snapshot.recallEnglishWords("wang", 5, 1000);
    assertTrue(wangMatches.empty(), "Chinese phrase is not in English recall index");

    // 4. Dynamic delta English recall
    snapshot.recordSelection("microservices", "microservices", "", "", 2000);
    auto microMatches = snapshot.recallEnglishWords("micro", 5, 2000);
    assertTrue(!microMatches.empty(), "delta English term is recalled by prefix");
    assertTrue(microMatches[0].text == "microservices", "delta English term matches microservices");
}

void testPhraseFormationTrackerUnit() {
    // 1. Intra-composition test: wangshaolong -> 王 + 绍 + 龙
    {
        modernime::core::PhraseFormationTracker tracker(5);
        tracker.recordPartialSelection("王", "wang", "wangshaolong", 4);
        tracker.recordPartialSelection("绍", "shao", "shaolong", 4);
        auto formed = tracker.recordFinalSelection("龙", "long", "long", 4);
        assertTrue(formed.has_value(), "intra-composition phrase formation succeeds");
        assertTrue(formed->text == "王绍龙", "formed text matches 王绍龙");
        assertTrue(formed->fullPinyin == "wang'shao'long", "formed pinyin matches wang'shao'long");
        assertTrue(formed->rawKeystrokes == "wangshaolong", "raw keystrokes match wangshaolong");

        // After completion, buffer is empty
        auto emptyFinal = tracker.recordFinalSelection("龙", "long", "long", 4);
        assertTrue(!emptyFinal.has_value(), "final selection on empty buffer returns nullopt");
    }

    // 2. Intra-composition cancel / backspace
    {
        modernime::core::PhraseFormationTracker tracker(5);
        tracker.recordPartialSelection("王", "wang", "wangshaolong", 4);
        tracker.cancelComposition();
        auto cancelled = tracker.recordFinalSelection("绍", "shao", "shaolong", 4);
        assertTrue(!cancelled.has_value(), "cancelled composition yields nullopt");
    }

    // 3. Inter-composition repeated sequences (threshold = 5)
    {
        modernime::core::PhraseFormationTracker tracker(5);
        for (int round = 1; round <= 5; ++round) {
            modernime::core::CommitEvent e1{"王", "wang", "wang", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 1000 + round * 10};
            modernime::core::CommitEvent e2{"绍", "shao", "shao", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 1000 + round * 10 + 1};
            modernime::core::CommitEvent e3{"龙", "long", "long", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 1000 + round * 10 + 2};

            auto f1 = tracker.recordCommit(e1);
            auto f2 = tracker.recordCommit(e2);
            auto f3 = tracker.recordCommit(e3);

            if (round < 5) {
                bool foundWangShaoLong = false;
                for (const auto &f : f3) {
                    if (f.text == "王绍龙") foundWangShaoLong = true;
                }
                assertTrue(!foundWangShaoLong, "王绍龙 not formed before threshold 5");
            } else {
                bool foundWangShaoLong = false;
                for (const auto &f : f3) {
                    if (f.text == "王绍龙") {
                        foundWangShaoLong = true;
                        assertTrue(f.fullPinyin == "wang'shao'long", "pinyin matches");
                        assertTrue(f.isInitialFormation, "isInitialFormation is true at threshold");
                        assertTrue(f.repetitionCount == 5, "repetitionCount is 5");
                    }
                }
                assertTrue(foundWangShaoLong, "王绍龙 formed at threshold 5");
            }
        }

        // Round 6: repetition continues to emit with isInitialFormation = false
        modernime::core::CommitEvent e1{"王", "wang", "wang", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 2000};
        modernime::core::CommitEvent e2{"绍", "shao", "shao", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 2001};
        modernime::core::CommitEvent e3{"龙", "long", "long", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 2002};
        tracker.recordCommit(e1);
        tracker.recordCommit(e2);
        auto f6 = tracker.recordCommit(e3);
        bool foundRound6 = false;
        for (const auto &f : f6) {
            if (f.text == "王绍龙") {
                foundRound6 = true;
                assertTrue(!f.isInitialFormation, "round 6 has isInitialFormation = false");
                assertTrue(f.repetitionCount == 6, "round 6 has repetitionCount = 6");
            }
        }
        assertTrue(foundRound6, "round 6 emits repetition promotion");
    }

    // 4. Interrupted by English or Raw
    {
        modernime::core::PhraseFormationTracker tracker(3);
        modernime::core::CommitEvent e1{"王", "wang", "wang", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 1000};
        tracker.recordCommit(e1);
        modernime::core::CommitEvent eRaw{"raw", "raw", "raw", modernime::core::CommitCandidateRole::Raw, false, 3, "", "", 1001};
        tracker.recordCommit(eRaw);
        modernime::core::CommitEvent e2{"绍", "shao", "shao", modernime::core::CommitCandidateRole::Chinese, false, 4, "", "", 1002};
        auto f2 = tracker.recordCommit(e2);
        assertTrue(f2.empty(), "interruption by raw resets sequence");
    }
}

void testContextAssociationAndSmoothing() {
    const auto path = testPath("context-association.sqlite3");
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "context association store opens");

    // 1. Single character context: 王 -> 绍
    assertTrue(store.recordSelection("绍", "shao", "王", "", 1000),
               "record selection of 绍 with context 王");
    auto snapshot1 = store.snapshot(1000);
    const double exact1 = snapshot1->contextBoost("绍", "shao", "王", "");
    assertTrue(exact1 > 0.6, "exact 1-char context gives strong boost");
    const double shifted1 = snapshot1->contextBoost("绍", "shao", "这是王", "");
    assertTrue(shifted1 > 0.5, "suffix 1-char context gives boost");
    const double diffContext1 = snapshot1->contextBoost("绍", "shao", "李", "");
    assertTrue(diffContext1 == 0.0, "unrelated context gives 0 boost");
    const double emptyContext1 = snapshot1->contextBoost("绍", "shao", "", "");
    assertTrue(emptyContext1 == 0.0, "empty context gives 0 context boost");

    // 2. Multi-character context: 王绍 -> 龙
    assertTrue(store.recordSelection("龙", "long", "王绍", "", 1000),
               "record selection of 龙 with context 王绍");
    auto snapshot2 = store.snapshot(1000);
    const double exact2 = snapshot2->contextBoost("龙", "long", "王绍", "");
    assertTrue(exact2 > 0.8, "exact 2-char context gives high boost");
    const double shifted2 = snapshot2->contextBoost("龙", "long", "公司王绍", "");
    assertTrue(shifted2 > 0.7, "suffix 2-char context gives high boost");
    const double diffContext2 = snapshot2->contextBoost("龙", "long", "张三", "");
    assertTrue(diffContext2 == 0.0, "unrelated context gives 0 boost");

    // 3. Frequency reinforcement
    assertTrue(store.recordSelection("龙", "long", "王绍", "", 1001),
               "second selection of 龙 with context 王绍");
    auto snapshot3 = store.snapshot(1001);
    const double repeatedBoost = snapshot3->contextBoost("龙", "long", "王绍", "");
    assertTrue(repeatedBoost > exact2, "repeated selection increases context boost");

    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testSchemaVersionAndBackup() {
    const auto path = testPath("schema-version.sqlite3");
    const auto backupPath = testPath("schema-version-backup.sqlite3");
    {
        modernime::core::LearningStore store(path);
        assertTrue(store.open(), "store opens successfully");
        assertTrue(store.schemaVersion() == 1, "store initializes user_version to 1");
        assertTrue(store.recordSelection("测试", "ceshi", {}, {}, 1000), "records selection");
        assertTrue(store.backupTo(backupPath), "creates backup via sqlite backup API");
    }
    {
        modernime::core::LearningStore backupStore(backupPath);
        assertTrue(backupStore.openReadOnly(), "backup opens read-only");
        assertTrue(backupStore.schemaVersion() == 1, "backup retains schema version 1");
        const auto snap = backupStore.snapshot();
        assertTrue(snap != nullptr && snap->hasPositiveFrequency("测试", "ceshi"), "backup contains learned entry");
    }
}

void testPublishedSnapshotDoesNotBuildRecallIndexOnFirstKey() {
    std::vector<modernime::core::LearningEntry> entries;
    entries.reserve(50000);
    for (int index = 0; index < 50000; ++index) {
        entries.push_back({"测试词" + std::to_string(index), "ni'hao",
                           {}, {}, 1, 1000, 0, false});
    }
    const modernime::core::LearningSnapshot snapshot(std::move(entries));
    // Publishing a new learning snapshot must not move its full index build
    // to the next keypress, even when the query has no matching learned word.
    const auto start = std::chrono::steady_clock::now();
    const auto recalled = snapshot.recallCandidates("bu'cun'zai", 1000);
    assertTrue(recalled.empty(), "unmatched query has no learned candidates");
    assertTrue(std::chrono::steady_clock::now() - start <
                   std::chrono::milliseconds(30),
               "first key on a published snapshot avoids rebuilding all learning entries");
}

void testFailedWriterQueueRetainsOnlyRecentEvents() {
    const auto path = testPath("writer-bounded-failure.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        sqlite3 *fault = nullptr;
        assertTrue(sqlite3_open(path.c_str(), &fault) == SQLITE_OK,
                   "bounded failure test opens storage fault injector");
        assertTrue(sqlite3_exec(fault,
            "CREATE TRIGGER fail_bounded_queue BEFORE INSERT ON learning_entries "
            "BEGIN SELECT RAISE(ABORT, 'disk failure'); END;",
            nullptr, nullptr, nullptr) == SQLITE_OK,
            "bounded queue test injects persistent write failure");
        writer.enqueueSelection("故障首词", "guzhang", {}, {}, 1);
        assertTrue(!writer.flush(), "initial fault leaves pending learning");
        for (int index = 0; index < 150; ++index) {
            writer.enqueueSelection("故障词" + std::to_string(index),
                                     "ceshi", {}, {}, index + 2);
        }
        assertTrue(!writer.flush(), "storage remains unavailable after many selections");
        assertTrue(sqlite3_exec(fault, "DROP TRIGGER fail_bounded_queue;",
                                nullptr, nullptr, nullptr) == SQLITE_OK,
                   "bounded queue test restores storage");
        sqlite3_close(fault);
        assertTrue(writer.flush(), "recent retained events persist after recovery");
    }
    modernime::core::LearningStore reopened(path);
    assertTrue(reopened.open(), "bounded failure database reopens");
    const auto snapshot = reopened.snapshot(200);
    assertTrue(snapshot->entry("故障首词", "guzhang", {}, {}) == nullptr &&
                   snapshot->entry("故障词49", "ceshi", {}, {}) == nullptr,
               "old events beyond queue capacity are discarded during failure");
    assertTrue(snapshot->entry("故障词50", "ceshi", {}, {}) != nullptr &&
                   snapshot->entry("故障词149", "ceshi", {}, {}) != nullptr,
               "the most recent 100 queued events survive storage recovery");
    reopened.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

void testFormedPhraseStatisticsStayBounded() {
    for (const int threshold : {1, 3}) {
        auto stats = std::make_shared<modernime::core::PhraseFormationStats>();
        modernime::core::PhraseFormationTracker tracker(threshold, stats);
        for (int index = 0; index < 600; ++index) {
            const auto codepoint = static_cast<unsigned int>(0x4e00 + index);
            std::string character;
            character.push_back(static_cast<char>(0xe0U | (codepoint >> 12)));
            character.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU)));
            character.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
            for (int repeat = 0; repeat < threshold; ++repeat) {
                tracker.resetCommitHistory();
                modernime::core::CommitEvent first;
                first.text = character;
                first.fullPinyin = "jia";
                first.role = modernime::core::CommitCandidateRole::Chinese;
                first.timestampMs = index * 10 + repeat + 1;
                tracker.recordCommit(first);
                auto second = first;
                second.text = "乙";
                second.fullPinyin = "yi";
                const auto formed = tracker.recordCommit(second);
                if (repeat == threshold - 1) {
                    assertTrue(!formed.empty(), "repeated pair forms a phrase");
                }
            }
            assertTrue(stats->sequenceStats.size() <= 500,
                       "formed sequences obey the 500-entry statistics bound");
        }
        assertTrue(!stats->sequenceStats.contains("一乙\x1fjia'yi"),
                   "old formed sequence is evicted when capacity is reached");
    }
}

void testLmdbKeyNormalizationNoDuplicateCount() {
    std::error_code error;
    const auto path = testPath("lmdb-norm.lmdb");
    std::filesystem::remove_all(path, error);

    const int64_t now = 1000000;
    modernime::core::LearningStore lmdb(path);
    assertTrue(lmdb.open(), "open LMDB store");
    assertTrue(lmdb.recordSelection("你好", "ni'hao", "", "", now), "record ni'hao in LMDB");
    const auto lmdbLoaded = lmdb.snapshot();
    assertTrue(lmdbLoaded != nullptr, "snapshot loaded from LMDB");

    modernime::core::LearningSnapshot incremental(*lmdbLoaded);
    incremental.recordSelection("你好", "ni'hao", "", "", now);

    int64_t total = 0;
    for (const auto &e : incremental.entries()) {
        total += e.frequency;
    }
    assertTrue(incremental.entries().size() == 1,
               "normalized key prevents duplicate rows across LMDB base and delta");
    assertTrue(total == 2, "total frequency increments to 2 without double-counting");
    lmdb.close();
    std::filesystem::remove_all(path, error);
}

void testPrefixRecallTopKByLearningBoost() {
    const int64_t now = 1000000;
    std::vector<modernime::core::LearningEntry> entries;
    for (auto py : {"zhong'a", "zhong'ai", "zhong'an", "zhong'ang", "zhong'ao",
                    "zhong'ba", "zhong'bai", "zhong'ban", "zhong'bang", "zhong'bao",
                    "zhong'bei", "zhong'ben", "zhong'beng", "zhong'bi", "zhong'bian",
                    "zhong'biao"}) {
        entries.push_back({"词" + std::to_string(entries.size()), py, "", "", 1, now, 0, false});
    }
    entries.push_back({"中作", "zhong'zuo", "", "", 1000, now, 0, false});
    assertTrue(entries.size() == 17, "17 entries created");

    modernime::core::LearningSnapshot prefix(entries);
    const auto recalled = prefix.recallCandidates("zhong", now);
    assertTrue(recalled.size() == 16, "prefix recall returns bounded top 16");
    const bool hasZhongZuo = std::any_of(recalled.begin(), recalled.end(),
        [](const auto &c) { return c.phrase == "中作"; });
    assertTrue(hasZhongZuo,
               "highest frequency entry '中作' is recalled in top 16 despite being 17th alphabetically");
}

void testContextBoostPenalizedByNegativeFeedback() {
    const int64_t now = 1000000;
    modernime::core::LearningSnapshot s;
    for (int i = 0; i < 10; ++i) {
        s.recordSelection("绍", "shao", "王", "", now);
    }
    const auto before = s.contextBoost("绍", "shao", "王", "");
    assertTrue(before > 1.0, "context boost active for frequent context pair");

    for (int i = 0; i < 100; ++i) {
        s.recordNegativeFeedback("绍", "shao");
    }
    const auto after = s.contextBoost("绍", "shao", "王", "");
    assertTrue(after == 0.0, "repeated negative feedback attenuates context boost to 0");
}

void testLearningStoreSmallCapacityBoundaryRetainsLimitExactForSqliteAndLmdb() {
    for (const auto &backendName : {"sqlite", "lmdb"}) {
        const std::string filename = std::string("capacity-bound.") + (std::string(backendName) == "lmdb" ? "lmdb" : "sqlite3");
        const auto path = testPath(filename);
        modernime::core::LearningStore store(path);
        store.setTotalEntryLimit(5);
        assertTrue(store.open(), "capacity store opens");
        for (int i = 0; i < 8; ++i) {
            assertTrue(store.recordSelection("词" + std::to_string(i), "ci", {}, {}, 1000 + i),
                       "entry records successfully");
            auto state = store.readState();
            assertTrue(state.has_value() && state->snapshot != nullptr, "state valid");
            const std::size_t expected = std::min(i + 1, 5);
            assertTrue(state->snapshot->entries().size() == expected,
                       "snapshot retains exactly expected capacity");
            if (store.personalEngine() != nullptr) {
                assertTrue(store.personalEngine()->GetTotalEntries() == expected,
                           "physical LMDB count retains exactly expected capacity");
            }
        }
        // Test reopening preserves exact capacity
        store.close();
        modernime::core::LearningStore reopened(path);
        reopened.setTotalEntryLimit(5);
        assertTrue(reopened.openReadOnly(), "reopened read-only");
        auto reopenedState = reopened.readState();
        assertTrue(reopenedState.has_value() && reopenedState->snapshot != nullptr, "reopened state valid");
        assertTrue(reopenedState->snapshot->entries().size() == 5, "reopened snapshot retains 5");
        reopened.close();

        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path.string() + "-wal", error);
        std::filesystem::remove(path.string() + "-shm", error);
        std::filesystem::remove(path.string() + "-lock", error);
    }
}

void testPersonalEngineStandaloneRetainsCustomPruneTarget() {
    modernime::core::EngineConfig cfg;
    cfg.max_dict_capacity = 5;
    cfg.prune_target_size = 4; // Explicit standalone PersonalEngine prune target
    modernime::core::PersonalEngine engine(cfg);
    uint32_t ts = 1700000000U;
    engine.SetClock([ts]() { return ts; });
    for (int i = 0; i < 5; ++i) {
        engine.RecordUsage("!u!term" + std::to_string(i), "词" + std::to_string(i), 1.0f);
    }
    assertTrue(engine.GetTotalEntries() == 5, "5 entries loaded before prune");
    // Trigger prune by adding 6th entry
    engine.RecordUsage("!u!term5", "词5", 1.0f);
    // Standalone PersonalEngine prunes down to prune_target_size (4)
    assertTrue(engine.GetTotalEntries() == 4, "standalone PersonalEngine prunes to custom prune_target_size (4)");
}

void testLearningStoreEntryCountLightweightAndFailure() {
    // 1. Unopened store returns nullopt
    const auto sqlitePath = testPath("entry-count-test.sqlite3");
    {
        modernime::core::LearningStore unopenStore(sqlitePath);
        assertTrue(!unopenStore.entryCount().has_value(),
                   "unopened store entryCount returns nullopt");
    }

    // 2. Open store returns count 0 initially
    {
        modernime::core::LearningStore store(sqlitePath);
        assertTrue(store.open(), "sqlite store opens");
        auto count = store.entryCount();
        assertTrue(count.has_value() && *count == 0, "initial entryCount is 0");

        assertTrue(store.recordSelection("词条一", "citiaoyi", {}, {}, 1000), "insert entry 1");
        assertTrue(store.recordSelection("词条二", "citiaoer", {}, {}, 1001), "insert entry 2");
        count = store.entryCount();
        assertTrue(count.has_value() && *count == 2, "entryCount is 2 after insertions");

        assertTrue(store.clear(), "store clears");
        count = store.entryCount();
        assertTrue(count.has_value() && *count == 0, "entryCount is 0 after clear");

        store.close();
        assertTrue(!store.entryCount().has_value(), "closed store entryCount returns nullopt");
    }
    std::filesystem::remove(sqlitePath);

    // 3. LMDB backend entry count
    const auto lmdbPath = testPath("entry-count-test.lmdb");
    {
        modernime::core::LearningStore lmdbStore(lmdbPath);
        assertTrue(!lmdbStore.entryCount().has_value(), "unopened lmdb store returns nullopt");
        assertTrue(lmdbStore.open(), "lmdb store opens");
        auto count = lmdbStore.entryCount();
        assertTrue(count.has_value() && *count == 0, "initial lmdb entryCount is 0");

        assertTrue(lmdbStore.recordSelection("词条一", "citiaoyi", {}, {}, 1000), "insert lmdb entry 1");
        assertTrue(lmdbStore.recordSelection("词条二", "citiaoer", {}, {}, 1001), "insert lmdb entry 2");
        count = lmdbStore.entryCount();
        assertTrue(count.has_value() && *count == 2, "lmdb entryCount is 2 after insertions");

        assertTrue(lmdbStore.clear(), "lmdb store clears");
        count = lmdbStore.entryCount();
        assertTrue(count.has_value() && *count == 0, "lmdb entryCount is 0 after clear");

        lmdbStore.close();
        assertTrue(!lmdbStore.entryCount().has_value(), "closed lmdb store entryCount returns nullopt");
    }
    std::filesystem::remove(lmdbPath);
}

} // namespace

int main() {
    testPublishedSnapshotDoesNotBuildRecallIndexOnFirstKey();
    testFailedWriterQueueRetainsOnlyRecentEvents();
    testFormedPhraseStatisticsStayBounded();
    testSchemaVersionAndBackup();
    testEnglishWordRecallIndex();
    testContextAssociationAndSmoothing();
    testPhraseFormationTrackerUnit();
    testCommitEventTrackingAndLearning();
    testIndependentRecallIndex();
    testExternalClearDiscardsRetainedDeltas();
    testWriterDetectsWalClearAndSerializesOwnWrites();
    testWalClearResumesNewWritesAfterRetryExhaustion();
    testFailedWriterPollDoesNotReopenWritableConnection();
    testFirstSelectionAfterUnobservedClearPersists();
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
    testDeltaOverlayCopyAndQueryPerformance();
    testLmdbKeyNormalizationNoDuplicateCount();
    testPrefixRecallTopKByLearningBoost();
    testContextBoostPenalizedByNegativeFeedback();
    testLearningStoreSmallCapacityBoundaryRetainsLimitExactForSqliteAndLmdb();
    testPersonalEngineStandaloneRetainsCustomPruneTarget();
    testLearningStoreEntryCountLightweightAndFailure();
    return EXIT_SUCCESS;
}
