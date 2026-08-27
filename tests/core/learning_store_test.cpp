#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
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
    assertTrue(snapshot->boostAt("你好", "nihao", "今", "，", 1000) > 0.0,
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
        std::string("乙") + '\x1f' + "a",
        std::string("甲") + '\x1f' + "a"};
    modernime::core::CandidateRanker::rank("a", candidates, previousOrder);
    assertTrue(candidates[1].stability_bonus > 0.0,
               "near-tied previous candidate receives stability bonus");
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
    assertTrue(snapshot->boostAt("错误词", "cuowuci", "", "", 1000) < 1.0,
               "negative feedback reduces the boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
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
    assertTrue(snapshot->boostAt("上下文词", "shangxiawen", "前文", "后文",
                                 1000) < 1.0,
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
    assertTrue(snapshot->boostAt("内存词", "neicun", {}, {}, 1000) > 0.0,
               "in-memory learning remains available after storage failure");
    assertTrue(!writer.flush(), "flush reports unavailable storage");
}

void testWriterFlushesSelectionBeforeReopen() {
    const auto path = testPath("writer.sqlite3");
    {
        modernime::core::LearningWriter writer(path);
        assertTrue(writer.enqueueSelection("写入词", "xieruci", {}, {}, 1000),
                   "writer accepts selection for a valid store");
        assertTrue(writer.flush(), "flush confirms durable learning write");
    }
    modernime::core::LearningStore store(path);
    assertTrue(store.open(), "flushed writer store reopens");
    const auto snapshot = store.snapshot();
    assertTrue(snapshot->boostAt("写入词", "xieruci", {}, {}, 1000) > 0.0,
               "flushed selection survives writer shutdown");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

} // namespace

int main() {
    testSelectionPersistsAcrossReopen();
    testFrequencyBonusIsBoundedAndMovesCandidate();
    testPreviousOrderAddsStabilityForNearTies();
    testNegativeFeedbackReducesLearningBoost();
    testMatchingContextRaisesCandidate();
    testBaseNegativeFeedbackAppliesToContextualSelection();
    testWriterReportsUnavailableStoreAndKeepsMemorySnapshot();
    testWriterFlushesSelectionBeforeReopen();
    return EXIT_SUCCESS;
}
