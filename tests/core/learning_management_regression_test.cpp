#include "modernime/core/learning_writer.h"
#include "modernime/core/learning_store.h"
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <future>
#include <chrono>
#include <thread>
#include <filesystem>
#include <unistd.h>

namespace {
using namespace modernime::core;
class LearningManagementRegression : public testing::TestWithParam<const char *> {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() / "modernime-management-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::filesystem::path path() const { return root / GetParam(); }
    std::filesystem::path root;
};
TEST_P(LearningManagementRegression, ForgetRemovesExactIdentityAndAllContextsInQueueOrder) {
    LearningWriter writer(path());
    writer.pauseWorkerForTest(true);
    ASSERT_TRUE(writer.enqueueSelection("重庆", "chong'qing", "前文", {}, 1000));
    ASSERT_TRUE(writer.enqueueSelection("重庆", "chongqing", {}, {}, 2000));
    ASSERT_TRUE(writer.enqueueSelection("重庆", "zhongqing", {}, {}, 3000));
    ASSERT_TRUE(writer.enqueueSelection("重情", "chongqing", {}, {}, 4000));
    auto previous = writer.snapshot();
    ASSERT_TRUE(writer.enqueueForget("重庆", "CHONG'QING"));
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("重庆", "chongqing"));
    EXPECT_TRUE(previous->hasPositiveFrequency("重庆", "chongqing"));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("重庆", "zhongqing"));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("重情", "chongqing"));
    writer.pauseWorkerForTest(false);
    ASSERT_TRUE(writer.flush());
    ASSERT_TRUE(writer.reload());
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("重庆", "chongqing"));
    ASSERT_TRUE(writer.enqueueSelection("重庆", "chongqing", {}, {}, 5000));
    ASSERT_TRUE(writer.flush());
    EXPECT_EQ(writer.snapshot()->entry("重庆", "chongqing", {}, {})->frequency, 1);
}
TEST_P(LearningManagementRegression, UnsuppressionPreservesFrequencyAndNegativeFeedback) {
    LearningWriter writer(path());
    ASSERT_TRUE(writer.enqueueSelection("你好", "nihao", "前文", {}, 1000));
    ASSERT_TRUE(writer.enqueueSuppression("你好", "nihao"));
    ASSERT_TRUE(writer.flush());
    auto previous = writer.snapshot();
    auto before = *previous->entry("你好", "nihao", {}, {});
    ASSERT_TRUE(writer.enqueueUnsuppression("你好", "NI'HAO"));
    EXPECT_FALSE(writer.snapshot()->isSuppressed("你好", "nihao"));
    EXPECT_TRUE(previous->isSuppressed("你好", "nihao"));
    ASSERT_TRUE(writer.flush());
    ASSERT_TRUE(writer.reload());
    auto after = writer.snapshot()->entry("你好", "nihao", {}, {});
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->frequency, before.frequency);
    EXPECT_EQ(after->negativeFeedback, before.negativeFeedback);
    EXPECT_EQ(writer.snapshot()->entry("你好", "nihao", "前文", {})->frequency, 1);
}
TEST_P(LearningManagementRegression, TrustedRestoreAdvancesRevisionAndPreservesAllFacts) {
    LearningWriter writer(path());
    ASSERT_TRUE(writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000));
    ASSERT_TRUE(writer.flush());
    auto previous = writer.snapshot();
    const auto revision = writer.resetRevision();
    std::vector<LearningEntry> restored{{"恢复词", "huifuci", "前文", "后文", 12345, 987654, 7, true}};
    ASSERT_TRUE(writer.restoreTrustedEntries(restored));
    EXPECT_GT(writer.resetRevision(), revision);
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("旧词", "jiuci"));
    EXPECT_TRUE(previous->hasPositiveFrequency("旧词", "jiuci"));
    const auto state = writer.exportConsistentState();
    ASSERT_TRUE(state.has_value());
    ASSERT_EQ(state->snapshot->entries().size(), 1u);
    const auto &entry = state->snapshot->entries().front();
    EXPECT_EQ(entry.frequency, 12345);
    EXPECT_EQ(entry.negativeFeedback, 7);
    EXPECT_EQ(entry.lastSelectedMs, 987654);
    EXPECT_TRUE(entry.suppressed);
    LearningStore independent(path());
    ASSERT_TRUE(independent.open());
    LearningEvent stale{LearningEvent::Kind::Selection, "旧词", "jiuci", {}, {}, 2000, revision};
    ASSERT_TRUE(independent.recordBatch({stale}));
    EXPECT_FALSE(independent.snapshot()->hasPositiveFrequency("旧词", "jiuci"));
}
TEST_P(LearningManagementRegression, ExpectedRestoreRejectsBothQueuedAndExternalChanges) {
    LearningWriter writer(path());
    ASSERT_TRUE(writer.enqueueSelection("你好", "nihao", {}, {}, 1000));
    auto expected = writer.exportConsistentState();
    ASSERT_TRUE(expected.has_value());
    const std::vector<LearningEntry> restored{{"恢复", "huifu", {}, {}, 12, 1000, 0, false}};
    // The consistent export is compatible with an optimistic normalized reading.
    ASSERT_TRUE(writer.restoreTrustedEntries(restored, *expected));
    expected = writer.exportConsistentState();
    ASSERT_TRUE(expected.has_value());
    ASSERT_TRUE(writer.enqueueSelection("新词", "xinci", {}, {}, 2000));
    EXPECT_FALSE(writer.restoreTrustedEntries(restored, *expected));
    ASSERT_TRUE(writer.flush());
    expected = writer.exportConsistentState();
    ASSERT_TRUE(expected.has_value());
    {
        LearningStore other(path());
        ASSERT_TRUE(other.open());
        ASSERT_TRUE(other.recordSelection("外部", "waibu", {}, {}, 3000));
    }
    EXPECT_FALSE(writer.restoreTrustedEntries(restored, *expected));
    ASSERT_TRUE(writer.reload());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("新词", "xinci"));
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("外部", "waibu"));
}
TEST_P(LearningManagementRegression, FailedRestoreLeavesExistingFactsAndRevision) {
    LearningWriter writer(path());
    ASSERT_TRUE(writer.enqueueSelection("原词", "yuanci", {}, {}, 1000));
    ASSERT_TRUE(writer.flush());
    const auto revision = writer.resetRevision();
    const std::vector<LearningEntry> invalid{{"重复", "chongfu", {}, {}, 9, 1000, 0, false},
                                            {"重复", "chong'fu", {}, {}, 10, 1000, 0, false}};
    EXPECT_FALSE(writer.restoreTrustedEntries(invalid));
    EXPECT_EQ(writer.resetRevision(), revision);
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("原词", "yuanci"));
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("重复", "chongfu"));
    ASSERT_TRUE(writer.flush());
    ASSERT_TRUE(writer.reload());
    EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("原词", "yuanci"));
}
TEST(LearningManagementConcurrencyRegression, SQLiteRestoreRetainsInputsAcceptedDuringTransaction) {
    std::string pattern = (std::filesystem::temp_directory_path() / "modernime-restore-XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{pattern};
    const auto databasePath = cleanup.root / "learning.sqlite3";
    LearningWriter writer(databasePath);
    ASSERT_TRUE(writer.enqueueSelection("旧词", "jiuci", {}, {}, 1000));
    ASSERT_TRUE(writer.flush());
    sqlite3 *blocker = nullptr;
    ASSERT_EQ(sqlite3_open(databasePath.c_str(), &blocker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(blocker, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
    const std::vector<LearningEntry> restored{{"恢复", "huifu", {}, {}, 12, 1000, 0, false}};
    auto result = std::async(std::launch::async, [&] { return writer.restoreTrustedEntries(restored); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!writer.isRestoringForTest() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool restoring = writer.isRestoringForTest();
    EXPECT_TRUE(restoring);
    if (restoring) {
        EXPECT_TRUE(writer.enqueueSelection("新输入", "xinshuru", {}, {}, 2000));
    }
    EXPECT_EQ(sqlite3_exec(blocker, "COMMIT;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(blocker);
    ASSERT_TRUE(result.get());
    ASSERT_TRUE(writer.flush());
    ASSERT_TRUE(writer.reload());
    EXPECT_FALSE(writer.snapshot()->hasPositiveFrequency("旧词", "jiuci"));
    EXPECT_EQ(writer.snapshot()->entry("恢复", "huifu", {}, {})->frequency, 12);
    ASSERT_NE(writer.snapshot()->entry("新输入", "xinshuru", {}, {}), nullptr);
    EXPECT_EQ(writer.snapshot()->entry("新输入", "xinshuru", {}, {})->frequency, 1);
}
INSTANTIATE_TEST_SUITE_P(Backends, LearningManagementRegression,
                        testing::Values("learning.sqlite3", "learning.lmdb"));
TEST(LearningManagementQueryRegression, SearchPaginationUsesStableIdentityAndAggregatesContexts) {
    LearningSnapshot snapshot({{"你好", "nihao", {}, {}, 2, 1000, 0, false},
                               {"你好", "ni'hao", "前文", {}, 3, 2000, 0, true},
                               {"你们", "nimen", {}, {}, 1, 3000, 0, false}});
    LearningQuery query;
    query.search = "NI";
    query.limit = 1;
    auto first = snapshot.queryLearningEntries(query);
    ASSERT_EQ(first.total, 2u);
    ASSERT_EQ(first.records.size(), 1u);
    EXPECT_EQ(first.records.front().phrase, "你好");
    EXPECT_EQ(first.records.front().frequency, 5);
    EXPECT_EQ(first.records.front().contextCount, 2u);
    query.suppressed = true;
    auto suppressed = snapshot.queryLearningEntries(query);
    ASSERT_EQ(suppressed.total, 1u);
    EXPECT_TRUE(suppressed.records.front().suppressed);
    query.suppressed.reset();
    query.offset = 1;
    EXPECT_EQ(snapshot.queryLearningEntries(query).records.front().phrase, "你们");
}
} // namespace
