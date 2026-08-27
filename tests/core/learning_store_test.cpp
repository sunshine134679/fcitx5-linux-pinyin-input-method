#include "modernime/core/candidate_ranker.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/learning_writer.h"

#include <cstdlib>
#include <cmath>
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
    assertTrue(snapshot->boostAt("错误词", "cuowuci", "", "", 1000) < 1.0,
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
    assertTrue(snapshot->boostAt("从未选过", "congweixuanguo", {}, {}, 1000) <=
                   0.0,
               "negative-only feedback never creates a positive learning boost");
    store.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testCorruptedLearningCountersRemainFinite() {
    const std::vector<modernime::core::LearningEntry> entries{
        {"损坏频次", "sunhuaici", {}, {}, -3, -1000, 0},
        {"损坏负反馈", "sunfankuici", {}, {}, 0, 0, -3}};
    const modernime::core::LearningSnapshot snapshot(entries);
    const auto frequencyBoost =
        snapshot.boostAt("损坏频次", "sunhuaici", {}, {}, 1000);
    const auto feedbackBoost =
        snapshot.boostAt("损坏负反馈", "sunfankuici", {}, {}, 1000);
    assertTrue(std::isfinite(frequencyBoost) && frequencyBoost == 0.0 &&
                   std::isfinite(feedbackBoost) && feedbackBoost == 0.0,
               "negative persisted counters are ignored safely");
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

} // namespace

int main() {
    testSelectionPersistsAcrossReopen();
    testFrequencyBonusIsBoundedAndMovesCandidate();
    testPreviousOrderAddsStabilityForNearTies();
    testCandidateOrderKeyIsSharedByRankingLayers();
    testDecoderScoreIsNormalizedAsASecondarySignal();
    testNegativeFeedbackReducesLearningBoost();
    testNegativeFeedbackWithoutSelectionDoesNotCreatePositiveBoost();
    testCorruptedLearningCountersRemainFinite();
    testMatchingContextRaisesCandidate();
    testBaseNegativeFeedbackAppliesToContextualSelection();
    testWriterReportsUnavailableStoreAndKeepsMemorySnapshot();
    testWriterFlushesSelectionBeforeReopen();
    testLearningBatchPersistsAsOneLogicalUpdate();
    return EXIT_SUCCESS;
}
