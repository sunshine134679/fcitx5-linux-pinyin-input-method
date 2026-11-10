#include "modernime/core/persistent_clipboard_history.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace modernime::core {
namespace {
class ClipboardAsyncRegression : public ::testing::Test {
protected:
    void SetUp() override {
        char tmp[] = "/tmp/modernime-clipboard-batch-gtest-XXXXXX";
        ASSERT_NE(::mkdtemp(tmp), nullptr);
        root = tmp;
        path = root / "history.bin";
    }
    void TearDown() override { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path root, path;
};
TEST_F(ClipboardAsyncRegression, SameTopFirstObservationDoesNotBecomePending) {
    ClipboardHistory seed; seed.observe("A"); ASSERT_TRUE(seed.save(path));
    PersistentClipboardHistory history(path); ASSERT_TRUE(history.load());
    ClipboardObservationBatch batch; batch.observe("A");
    EXPECT_FALSE(history.observeBatch(batch));
    seed.clear(); ASSERT_TRUE(seed.save(path));
    ASSERT_TRUE(history.flush()); ASSERT_TRUE(seed.load(path));
    EXPECT_TRUE(seed.entries().empty());
}
TEST_F(ClipboardAsyncRegression, CompactedPredecessorDoesNotLosePendingEligibility) {
    ClipboardHistory seed; seed.observe("60"); ASSERT_TRUE(seed.save(path));
    PersistentClipboardHistory history(path); ASSERT_TRUE(history.load());
    std::filesystem::create_directory(path.string() + ".tmp");
    ClipboardObservationBatch batch;
    for (int i = 0; i < 90; ++i) batch.observe(std::to_string(i));
    std::string error;
    ASSERT_TRUE(history.observeBatch(batch, &error)); ASSERT_FALSE(error.empty());
    std::filesystem::remove(path.string() + ".tmp");
    seed.clear(); ASSERT_TRUE(seed.save(path)); ASSERT_TRUE(history.flush());
    ASSERT_TRUE(seed.load(path)); ASSERT_EQ(seed.entries().size(), 30);
    EXPECT_EQ(seed.entries().front(), "89"); EXPECT_EQ(seed.entries().back(), "60");
}
TEST_F(ClipboardAsyncRegression, FreshRecopyAfterClearIgnoresOldSynchronousIdentity) {
    PersistentClipboardHistory history(path); ASSERT_TRUE(history.load());
    ASSERT_TRUE(history.observe("A"));
    ClipboardObservationTracker tracker; tracker.setBaseline("B");
    ClipboardHistory cleared; ASSERT_TRUE(cleared.save(path));
    ClipboardObservationBatch batch;
    ASSERT_TRUE(tracker.accept("A")); batch.observe("A");
    ASSERT_TRUE(history.reloadIfExternallyChanged());
    ASSERT_TRUE(history.observeBatch(batch));
    ASSERT_TRUE(cleared.load(path));
    EXPECT_EQ(cleared.entries(), std::vector<std::string>{"A"});
}
}
} // namespace modernime::core
