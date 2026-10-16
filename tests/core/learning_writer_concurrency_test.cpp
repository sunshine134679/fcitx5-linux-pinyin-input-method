#include "modernime/core/commit_event.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/pinyin_match.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using modernime::core::CommitCandidateRole;
using modernime::core::CommitEvent;
using modernime::core::LearningStore;
using modernime::core::LearningWriter;
using modernime::core::PinyinMatchPolicy;

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

} // namespace
