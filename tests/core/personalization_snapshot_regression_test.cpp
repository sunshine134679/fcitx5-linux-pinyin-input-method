#include "modernime/core/learning_snapshot.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <barrier>
#include <thread>
#include <array>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

namespace {
using namespace modernime::core;
std::vector<LearningEntry> largeVocabulary(int count = 50000) {
    std::vector<LearningEntry> result;
    for (int i = 0; i < count; ++i) {
        std::string word = "term";
        int value = i;
        for (int n = 0; n < 5; ++n) { word += char('a' + value % 26); value /= 26; }
        result.push_back({word, word, {}, {}, 1, 1000, 0, false});
    }
    return result;
}
TEST(PersonalizationSnapshotRegression, RepeatedPublicationNeverRebuildsWholeVocabulary) {
    auto current = std::make_shared<LearningSnapshot>(largeVocabulary());
    const auto original = current;
    double slowestMs = 0;
    for (int i = 0; i < 128; ++i) {
        const auto start = std::chrono::steady_clock::now();
        auto next = std::make_shared<LearningSnapshot>(*current);
        next->recordSelection("你好", "nihao", {}, {}, 2000 + i);
        EXPECT_EQ(next->recallCandidates("nihao", 2000 + i).size(), 1u);
        current = std::move(next);
        slowestMs = std::max(slowestMs, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
    EXPECT_LT(slowestMs, 30.0) << "full vocabulary merge or lazy index build on input";
    EXPECT_FALSE(original->hasPositiveFrequency("你好", "nihao"));
    EXPECT_EQ(current->entry("你好", "nihao", {}, {})->frequency, 128);
    const auto start = std::chrono::steady_clock::now();
    current->recordSuppression("你好", "nihao");
    EXPECT_TRUE(current->recallCandidates("nihao", 3000).empty());
    const double suppressionMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(suppressionMs, 30.0);
}
TEST(PersonalizationSnapshotRegression, FullCapacityInsertionEvictsLocallyAndKeepsOldSnapshot) {
    auto current = std::make_shared<LearningSnapshot>(largeVocabulary(200000));
    const auto original = current;
    double slowestMs = 0;
    for (int i = 0; i < 4; ++i) {
        const auto start = std::chrono::steady_clock::now();
        auto next = std::make_shared<LearningSnapshot>(*current);
        next->recordSelection("新词" + std::to_string(i), "xin'ci", {}, {}, 2000 + i);
        current = std::move(next);
        slowestMs = std::max(slowestMs, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
    EXPECT_LT(slowestMs, 150.0) << "capacity eviction must not rebuild 200k entries (tsan)";
#else
    EXPECT_LT(slowestMs, 80.0) << "capacity eviction must not rebuild 200k entries";
#endif
    EXPECT_EQ(current->entries().size(), kMaxLearningEntries);
    EXPECT_EQ(original->entries().size(), kMaxLearningEntries);
    EXPECT_FALSE(original->hasPositiveFrequency("新词0", "xin'ci"));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(current->hasPositiveFrequency("新词" + std::to_string(i), "xin'ci"));
}
TEST(PersonalizationSnapshotRegression, PrefixScanUsesGlobalLexicalOrderAcrossShards) {
    std::vector<LearningEntry> entries;
    for (int i = 0; entries.size() < 256; ++i) {
        auto phrase = "候选词" + std::to_string(i);
        if (std::hash<std::string_view>{}(phrase) % 512 == 0) {
            entries.push_back({phrase, "wang'ba", {}, {}, 1, 1000, 0, false});
        }
    }
    std::string target = "目标";
    while (std::hash<std::string_view>{}(target) % 512 == 0) target += "词";
    entries.push_back({target, "wang'a", {}, {}, 10000, 1000, 0, false});
    LearningSnapshot snapshot(std::move(entries));
    const auto recalled = snapshot.recallCandidates("wang", 1000);
    EXPECT_TRUE(std::any_of(recalled.begin(), recalled.end(), [&](const auto &item) { return item.phrase == target; }));
    EXPECT_LE(recalled.size(), 16u);
}
TEST(PersonalizationSnapshotRegression, DigraphInitialsRemainIndexedAfterPublication) {
    LearningSnapshot snapshot;
    snapshot.recordSelection("王绍龙", "wang'shao'long", {}, {}, 1000);
    for (const auto *input : {"wsl", "wshl"}) {
        const auto recalled = snapshot.recallCandidates(input, 1000);
        EXPECT_TRUE(std::any_of(recalled.begin(), recalled.end(), [](const auto &item) { return item.phrase == "王绍龙"; }));
    }
    snapshot.recordSelection("组合", "zu'he", {}, {}, 1000);
    const auto exactInitials = snapshot.recallCandidates("zh", 1000);
    EXPECT_TRUE(std::any_of(exactInitials.begin(), exactInitials.end(), [](const auto &item) { return item.phrase == "组合"; }));
}
TEST(PersonalizationSnapshotRegression, ConcurrentFirstMaterializationIsImmutable) {
    const LearningSnapshot snapshot(largeVocabulary(5000));
    const LearningSnapshot copy(snapshot);
    std::barrier start(8);
    std::array<std::size_t, 8> sizes{};
    std::array<std::thread, 8> readers;
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readers[i] = std::thread([&, i] {
            start.arrive_and_wait();
            sizes[i] = (i % 2 == 0 ? snapshot : copy).entries().size();
        });
    }
    for (auto &reader : readers) reader.join();
    for (auto size : sizes) EXPECT_EQ(size, 5000u);
    EXPECT_EQ(snapshot.entries().size(), copy.entries().size());
}
TEST(PersonalizationSnapshotRegression, StartupStorageFailureRetainsLearningForRecovery) {
    const auto root = std::filesystem::temp_directory_path() /
        ("modernime-startup-recovery-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto blocker = root / "blocked";
    std::ofstream(blocker) << "not a directory";
    const auto database = blocker / "learning.sqlite3";
    {
        LearningWriter writer(database);
        EXPECT_FALSE(writer.enqueueSelection("恢复", "huifu", {}, {}, 1000));
        EXPECT_FALSE(writer.flush());
        EXPECT_TRUE(writer.snapshot()->hasPositiveFrequency("恢复", "huifu"));
        std::filesystem::remove(blocker);
        std::filesystem::create_directory(blocker);
        EXPECT_TRUE(writer.flush());
    }
    LearningStore reopened(database);
    ASSERT_TRUE(reopened.open());
    EXPECT_TRUE(reopened.snapshot()->hasPositiveFrequency("恢复", "huifu"));
    reopened.close();
    std::filesystem::remove_all(root);
}
TEST(PersonalizationSnapshotRegression, ExactEntryDoesNotReturnEnglishPrefixAlias) {
    LearningSnapshot snapshot({
        {"garage", "gara", {}, {}, 3, 1000, 0, false},
        {"garage", "garage", {}, {}, 1, 2000, 0, false}});
    const auto *exact = snapshot.entry("garage", "garage", {}, {});
    ASSERT_NE(exact, nullptr);
    EXPECT_EQ(exact->pinyin, "garage");
    EXPECT_EQ(exact->frequency, 1);
    const auto words = snapshot.recallEnglishWords("gar", 5, 2000);
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words.front().frequency, 4);
}
TEST(PersonalizationSnapshotRegression, LegacyEnglishPrefixKeysRemainLearned) {
    LearningSnapshot snapshot({{"garage", "gara", {}, {}, 3, 1000, 0, false}});
    auto words = snapshot.recallEnglishWords("gar", 5, 1000);
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words.front().text, "garage");
    EXPECT_EQ(words.front().frequency, 3);
    snapshot.recordSuppression("garage", "garage");
    EXPECT_TRUE(snapshot.recallEnglishWords("gar", 5, 1000).empty());
}
}
