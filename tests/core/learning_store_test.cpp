#include "modernime/core/candidate_ranker.h"
#include "modernime/core/learning_store.h"

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

} // namespace

int main() {
    testSelectionPersistsAcrossReopen();
    testFrequencyBonusIsBoundedAndMovesCandidate();
    testNegativeFeedbackReducesLearningBoost();
    return EXIT_SUCCESS;
}
