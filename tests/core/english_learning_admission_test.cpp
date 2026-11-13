#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/learning_store.h"
#include "modernime/fcitx5/engine.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unistd.h>

namespace {
using namespace modernime;
constexpr std::int64_t kNow = 2'000'000'000'000;
constexpr std::int64_t kYear = 365LL * 24 * 3600 * 1000;

bool contains(const std::vector<core::RecalledEnglishWord> &words,
              std::string_view text) {
    return std::any_of(words.begin(), words.end(), [&](const auto &word) {
        return word.text == text;
    });
}

std::pair<std::string, std::string> collidingCaseVariants() {
    std::unordered_map<std::size_t, std::string> buckets;
    const std::string spelling = "microservices";
    for (unsigned mask = 0; mask < (1u << spelling.size()); ++mask) {
        auto word = spelling;
        for (std::size_t i = 0; i < word.size(); ++i)
            if ((mask & (1u << i)) != 0)
                word[i] = static_cast<char>(word[i] - ('a' - 'A'));
        const auto bucket = std::hash<std::string_view>{}(word) % 512;
        const auto [it, inserted] = buckets.emplace(bucket, word);
        if (!inserted) {
            auto first = it->second;
            if (word < first) std::swap(first, word);
            return {first, word};
        }
    }
    ADD_FAILURE() << "case variants must share one of the 512 snapshot shards";
    return {};
}

TEST(EnglishLearningAdmission, CaseVariantObservationDoesNotHideConfirmedSpellingInSameShard) {
    const auto [observation, confirmed] = collidingCaseVariants();
    ASSERT_FALSE(observation.empty());
    ASSERT_NE(observation, confirmed);
    std::vector<core::LearningEntry> entries{
        {observation, "microservices", {}, {}, 1, kNow, 0, false},
        {confirmed, "microservices", {}, {}, 20, kNow, 0, false}};
    for (int order = 0; order < 2; ++order) {
        SCOPED_TRACE(order);
        const core::LearningSnapshot snapshot(entries);
        const auto recalled = snapshot.recallEnglishWords("micro", 3, kNow);
        ASSERT_EQ(recalled.size(), 1u);
        EXPECT_EQ(recalled.front().text, confirmed);
        EXPECT_EQ(recalled.front().frequency, 20);
        EXPECT_EQ(snapshot.entry(observation, "microservices", {}, {})->frequency, 1);
        std::reverse(entries.begin(), entries.end());
    }
}

TEST(EnglishLearningAdmission, GlobalRecencyIsAppliedBeforeShardTruncation) {
    const auto shard = std::hash<std::string_view>{}("micro") % 512;
    std::vector<core::LearningEntry> entries{
        {"micro", "micro", {}, {}, 2, kNow - kYear, 0, false}};
    for (unsigned suffix = 0; entries.size() < 18; ++suffix) {
        std::string word = "microservices_population_";
        auto code = suffix;
        for (int n = 0; n < 6; ++n) {
            word += static_cast<char>('a' + code % 26);
            code /= 26;
        }
        if (std::hash<std::string_view>{}(word) % 512 == shard)
            entries.push_back({word, word, {}, {}, 100, kNow - kYear, 0, false});
    }
    const std::string recent = "micro_recently_rejected";
    ASSERT_NE(std::hash<std::string_view>{}(recent) % 512, shard);
    entries.push_back({recent, recent, {}, {}, 2, kNow, 20, false});
    const core::LearningSnapshot snapshot(entries);
    const auto recalled = snapshot.recallEnglishWords("micro", 3, kNow);
    ASSERT_EQ(recalled.size(), 3u);
    // All older words receive the same dormancy discount. The complete
    // spelling must survive even if long high-frequency words share its shard.
    EXPECT_EQ(recalled.front().text, "micro");
    EXPECT_DOUBLE_EQ(recalled.front().score,
                     snapshot.boostAt("micro", "micro", kNow) *
                         std::exp(-static_cast<double>(kYear) /
                                  (12.0 * 3600 * 1000 / std::log(2.0))) + 2.0);
}

TEST(EnglishLearningAdmission, IndexedRankingMatchesAllEligibleEvidenceBeforeAndAfterDelta) {
    std::vector<core::LearningEntry> entries;
    std::vector<std::string> words;
    for (unsigned i = 0; i < 900; ++i) {
        std::string word = i % 19 == 0 ? "Micro_" : "micro_";
        auto code = i;
        for (int n = 0; n < 3; ++n) { word += static_cast<char>('a' + code % 26); code /= 26; }
        words.push_back(word);
        const auto last = i % 37 == 0 ? 0 : kNow - static_cast<std::int64_t>(i % 90) * 3600 * 1000;
        const auto frequency = 1 + i % 40;
        entries.push_back({word, word, i % 3 == 0 ? "context" : "", {}, frequency,
                           last, i % 11, i % 31 == 0});
        if (i % 4 == 0)
            entries.push_back({word, "micro", "legacy context", {}, 1, last, 0, false});
    }
    core::LearningSnapshot snapshot(entries);
    const auto frozen = snapshot;
    for (int phase = 0; phase < 2; ++phase) {
        SCOPED_TRACE(phase);
        for (const auto prefix : {"mi", "micro", "micro_a", "micro_b", "micro_aa", "micro_aaa"}) {
            for (const auto now : {kNow - kYear, kNow, kNow + kYear}) {
                std::map<std::string, std::pair<std::int64_t, std::int64_t>> facts;
                for (const auto &item : snapshot.entries()) {
                    auto &word = facts[item.phrase];
                    word.first += std::max<std::int64_t>(0, item.frequency);
                    word.second = std::max(word.second, item.lastSelectedMs);
                }
                std::vector<core::RecalledEnglishWord> expected;
                std::int64_t latest = 0;
                double latestBoost = 0;
                for (const auto &[word, fact] : facts) {
                    auto lower = word;
                    lower[0] = 'm';
                    if (!lower.starts_with(prefix) || fact.first < 2 || snapshot.isSuppressed(word, word)) continue;
                    const double boost = snapshot.boostAt(word, word, now);
                    expected.push_back({word, fact.first, fact.second, 0, boost});
                    if (fact.second > latest) { latest = fact.second; latestBoost = std::max(0.0, boost); }
                    else if (fact.second == latest) latestBoost = std::max(latestBoost, boost);
                }
                for (auto &item : expected) {
                    double boost = item.baseBoost;
                    if (latest > 0 && item.lastSelectedMs > 0 && item.lastSelectedMs < latest) {
                        double dormant = std::exp(-static_cast<double>(latest - item.lastSelectedMs) /
                                                 (12.0 * 3600 * 1000 / 0.6931471805599453));
                        if (latestBoost >= 1.8)
                            dormant = std::min(dormant, 1.0 - 0.30 * std::min(1.0, (latestBoost - 1.8) / (3.9 - 1.8)));
                        boost *= dormant;
                    }
                    item.score = boost + static_cast<double>(std::string_view(prefix).size()) / item.text.size();
                    if (item.text.size() == std::string_view(prefix).size()) item.score += 1;
                }
                std::sort(expected.begin(), expected.end(), [](const auto &a, const auto &b) {
                    if (a.score != b.score) return a.score > b.score;
                    if (a.frequency != b.frequency) return a.frequency > b.frequency;
                    if (a.lastSelectedMs != b.lastSelectedMs) return a.lastSelectedMs > b.lastSelectedMs;
                    return a.text < b.text;
                });
                if (expected.size() > 7) expected.resize(7);
                const auto actual = snapshot.recallEnglishWords(prefix, 7, now);
                ASSERT_EQ(actual.size(), expected.size()) << prefix << " at " << now;
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    EXPECT_EQ(actual[i].text, expected[i].text) << prefix << " at " << now;
                    EXPECT_EQ(actual[i].frequency, expected[i].frequency);
                    EXPECT_DOUBLE_EQ(actual[i].score, expected[i].score);
                    EXPECT_DOUBLE_EQ(actual[i].baseBoost, expected[i].baseBoost);
                }
            }
        }
        if (phase == 0) {
            for (std::size_t i = 0; i < words.size(); i += 7) {
                snapshot.recordSelection(words[i], words[i], {}, {}, kNow + static_cast<std::int64_t>(i));
                if (i % 3 == 0) snapshot.recordSuppression(words[i], words[i]);
                else snapshot.recordNegativeFeedback(words[i], words[i]);
            }
            EXPECT_TRUE(core::learningEntriesEqual(frozen.entries(), entries));
        }
    }
}

TEST(EnglishLearningAdmission, SingleUnknownObservationRemainsStoredWithoutRecall) {
    for (const std::string word : {"sudoi", "dockerx", "gnueabihf", "microservices"}) {
        SCOPED_TRACE(word);
        ASSERT_FALSE(core::EnglishDictionary::isEnglishWord(word));
        core::LearningSnapshot observed;
        observed.recordSelection(word, word, {}, {}, kNow);
        ASSERT_NE(observed.entry(word, word, {}, {}), nullptr);
        EXPECT_EQ(observed.entry(word, word, {}, {})->frequency, 1);
        // Loading an existing one-hit record follows the same policy as the
        // optimistic delta. Neither advancing time nor requesting the exact
        // word may turn an observation into an automatic completion.
        const core::LearningSnapshot loaded(observed.entries());
        for (const auto *snapshot : std::vector<const core::LearningSnapshot *>{&observed, &loaded}) {
            for (const auto time : {kNow, kNow + 2 * kYear}) {
                for (std::size_t length = 2; length <= word.size(); ++length) {
                    const auto prefix = word.substr(0, length);
                    EXPECT_FALSE(contains(snapshot->recallEnglishWords(prefix, 3, time), word))
                        << "prefix=" << prefix << " now=" << time;
                }
            }
            EXPECT_EQ(snapshot->entry(word, word, {}, {})->frequency, 1);
        }
    }
}

TEST(EnglishLearningAdmission, RepeatedUseAdmitsAcrossContextsAndLegacyKeysBeforeTopLimit) {
    core::LearningSnapshot snapshot({
        {"gnueabihf", "gnu", {}, {}, 1, kNow - 1000, 0, false},
        {"micro_a", "micro_a", {}, {}, 1, kNow, 0, false},
        {"micro_b", "micro_b", {}, {}, 1, kNow, 0, false},
        {"micro_c", "micro_c", {}, {}, 1, kNow, 0, false},
        {"microservices", "microservices", {}, {}, 2, kNow - kYear, 0, false}});
    EXPECT_TRUE(snapshot.recallEnglishWords("gnue", 3, kNow).empty());
    const auto oldSnapshot = snapshot;
    snapshot.recordSelection("gnueabihf", "gnueabihf", "toolchain", {}, kNow);
    const auto recalled = snapshot.recallEnglishWords("gnue", 3, kNow);
    ASSERT_EQ(recalled.size(), 1u);
    EXPECT_EQ(recalled.front().text, "gnueabihf");
    EXPECT_EQ(recalled.front().frequency, 2);
    EXPECT_TRUE(oldSnapshot.recallEnglishWords("gnue", 3, kNow).empty());
    const auto limited = snapshot.recallEnglishWords("micro", 1, kNow);
    ASSERT_EQ(limited.size(), 1u);
    EXPECT_EQ(limited.front().text, "microservices");
    EXPECT_EQ(snapshot.entries().size(), 6u);
}

TEST(EnglishLearningAdmission, BuiltinFirstUseAndConfirmedWordKeepSuppressionAndDecay) {
    core::LearningSnapshot snapshot;
    ASSERT_TRUE(core::EnglishDictionary::isEnglishWord("gnupg"));
    snapshot.recordSelection("gnupg", "gnupg", {}, {}, kNow);
    EXPECT_TRUE(contains(snapshot.recallEnglishWords("gnu", 3, kNow), "gnupg"));
    snapshot.recordSelection("gnueabihf", "gnueabihf", {}, {}, kNow);
    snapshot.recordSelection("gnueabihf", "gnueabihf", {}, {}, kNow);
    const auto fresh = snapshot.recallEnglishWords("gnue", 3, kNow);
    const auto aged = snapshot.recallEnglishWords("gnue", 3, kNow + kYear);
    ASSERT_EQ(fresh.size(), 1u);
    ASSERT_EQ(aged.size(), 1u);
    EXPECT_LT(aged.front().baseBoost, fresh.front().baseBoost);
    snapshot.recordNegativeFeedback("gnueabihf", "gnueabihf");
    EXPECT_LT(snapshot.recallEnglishWords("gnue", 3, kNow).front().baseBoost,
              fresh.front().baseBoost);
    snapshot.recordSuppression("gnueabihf", "gnueabihf");
    EXPECT_TRUE(snapshot.recallEnglishWords("gnue", 3, kNow).empty());
    EXPECT_EQ(snapshot.entry("gnueabihf", "gnueabihf", {}, {})->frequency, 2);
    snapshot.recordUnsuppression("gnueabihf", "gnueabihf");
    EXPECT_TRUE(contains(snapshot.recallEnglishWords("gnue", 3, kNow), "gnueabihf"));
}

TEST(EnglishLearningAdmission, BuiltinConsonantTermsRecallAfterFirstUseWithoutChangingArbitration) {
    for (const auto &[word, prefix] : std::vector<std::pair<std::string, std::string>>{
             {"http", "ht"}, {"html", "htm"}, {"sql", "sq"}, {"HTTP", "HT"}}) {
        SCOPED_TRACE(word);
        // These entries exist in the built-in short-word table, but the
        // arbitration heuristic intentionally rejects vowel-less spellings.
        ASSERT_GT(core::EnglishDictionary::wordPopularityWeight(word == "HTTP" ? "http" : word), 0.0);
        EXPECT_FALSE(core::EnglishDictionary::isEnglishWord(word));
        core::LearningSnapshot observed;
        observed.recordSelection(word, word, {}, {}, kNow);
        EXPECT_TRUE(contains(observed.recallEnglishWords(prefix, 3, kNow), word));
        const core::LearningSnapshot loaded(observed.entries());
        EXPECT_TRUE(contains(loaded.recallEnglishWords(prefix, 3, kNow), word));
        ASSERT_NE(loaded.entry(word, word, {}, {}), nullptr);
        EXPECT_EQ(loaded.entry(word, word, {}, {})->frequency, 1);
    }
}

class EnglishLearningAdmissionPersistence : public ::testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-english-admission-XXXXXX").string();
        const auto created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

TEST_F(EnglishLearningAdmissionPersistence, OldSingleObservationReopensAndSecondUseAdmits) {
    for (const bool lmdb : {false, true}) {
        SCOPED_TRACE(lmdb ? "LMDB" : "SQLite");
        const auto path = root / (lmdb ? "lmdb" : "sqlite") /
                          (lmdb ? "learning.lmdb" : "learning.sqlite3");
        std::filesystem::create_directories(path.parent_path());
        {
            core::LearningStore store(path);
            ASSERT_TRUE(store.open());
            ASSERT_TRUE(store.recordSelection("sudoi", "sudoi", {}, {}, kNow));
        }
        {
            core::LearningStore reopened(path);
            ASSERT_TRUE(reopened.open());
            const auto old = reopened.snapshot();
            ASSERT_NE(old->entry("sudoi", "sudoi", {}, {}), nullptr);
            EXPECT_EQ(old->entry("sudoi", "sudoi", {}, {})->frequency, 1);
            EXPECT_TRUE(old->recallEnglishWords("sudo", 3, kNow + kYear).empty());
            ASSERT_TRUE(reopened.recordSelection("sudoi", "sudoi", {}, {}, kNow + kYear));
            EXPECT_TRUE(contains(reopened.snapshot()->recallEnglishWords("sudo", 3,
                                                                       kNow + kYear), "sudoi"));
            EXPECT_EQ(reopened.snapshot()->entry("sudoi", "sudoi", {}, {})->frequency, 2);
            EXPECT_TRUE(old->recallEnglishWords("sudo", 3, kNow + kYear).empty());
        }
    }
}

struct AdmissionHost final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed = text; }
};

std::size_t rank(const core::CandidatePage &page, std::string_view text) {
    for (std::size_t i = 0; i < page.items.size(); ++i)
        if (page.items[i].text == text) return i;
    return page.items.size();
}

TEST_F(EnglishLearningAdmissionPersistence, CaseVariantObservationKeepsConfirmedCompletionAfterReopen) {
    const auto [observation, confirmed] = collidingCaseVariants();
    ASSERT_FALSE(observation.empty());
    for (const bool lmdb : {false, true}) {
        SCOPED_TRACE(lmdb ? "LMDB" : "SQLite");
        const auto directory = root / (lmdb ? "case-lmdb" : "case-sqlite");
        std::filesystem::create_directories(directory);
        pinyin::PinyinDataPaths paths;
        paths.learningStore = (directory / (lmdb ? "learning.lmdb" : "learning.sqlite3")).string();
        paths.userDictionary = (directory / "user.dict").string();
        paths.dictionaryPacks = (directory / "packs").string();
        paths.bundledDictionaryPacks = (directory / "bundled").string();
        {
            core::LearningStore seed(paths.learningStore);
            ASSERT_TRUE(seed.open());
            ASSERT_TRUE(seed.recordSelection(observation, "microservices", {}, {}, kNow));
            for (int i = 0; i < 20; ++i)
                ASSERT_TRUE(seed.recordSelection(confirmed, "microservices", {}, {}, kNow));
        }
        pinyin::PinyinCandidateProvider provider(paths);
        AdmissionHost host;
        fcitx5::ModernIMEController controller(host, &provider);
        for (const char key : std::string("micro"))
            ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
        const auto index = rank(controller.page(), confirmed);
        ASSERT_LT(index, controller.page().items.size());
        EXPECT_EQ(index, 0u);
        EXPECT_EQ(rank(controller.page(), observation), controller.page().items.size());
        EXPECT_EQ(controller.page().items[index].consumedInputBytes, 5u);
        ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Digit, 0, '1'}));
        EXPECT_EQ(host.committed, confirmed);
        EXPECT_TRUE(controller.page().rawInput.empty());
        ASSERT_TRUE(provider.flush());
        const auto snapshot = provider.sharedResources()->learning->snapshot();
        EXPECT_EQ(snapshot->entry(observation, "microservices", {}, {})->frequency, 1);
        EXPECT_EQ(snapshot->entry(confirmed, "microservices", {}, {})->frequency, 21);
    }
}

TEST_F(EnglishLearningAdmissionPersistence, BuiltinConsonantCompletionRemainsSelectableAfterOneUseAndReopen) {
    for (const bool lmdb : {false, true}) {
        for (const auto &[word, prefix] : std::vector<std::pair<std::string, std::string>>{
                 {"http", "ht"}, {"html", "htm"}}) {
            SCOPED_TRACE(word + (lmdb ? " LMDB" : " SQLite"));
            const auto directory = root / (word + (lmdb ? "-lmdb" : "-sqlite"));
            std::filesystem::create_directories(directory);
            pinyin::PinyinDataPaths paths;
            paths.learningStore = (directory / (lmdb ? "learning.lmdb" : "learning.sqlite3")).string();
            paths.userDictionary = (directory / "empty-user.dict").string();
            paths.dictionaryPacks = (directory / "empty-packs").string();
            paths.bundledDictionaryPacks = (directory / "empty-bundled-packs").string();
            paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
            paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
            {
                pinyin::PinyinCandidateProvider provider(paths);
                AdmissionHost host;
                fcitx5::ModernIMEController controller(host, &provider);
                controller.setContext("", "");
                for (const char key : word)
                    ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
                ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Enter}));
                ASSERT_EQ(host.committed, word);
                ASSERT_TRUE(provider.flush());
                const auto snapshot = provider.sharedResources()->learning->snapshot();
                ASSERT_NE(snapshot->entry(word, word, {}, {}), nullptr);
                ASSERT_EQ(snapshot->entry(word, word, {}, {})->frequency, 1);
                controller.setContext("", "");
                for (const char key : prefix)
                    ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
                const auto index = rank(controller.page(), word);
                ASSERT_LT(index, controller.page().items.size());
                EXPECT_EQ(index, 2u);
                EXPECT_EQ(controller.page().items[index].consumedInputBytes, prefix.size());
                ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Escape}));
            }
            pinyin::PinyinCandidateProvider reopened(paths);
            AdmissionHost host;
            fcitx5::ModernIMEController controller(host, &reopened);
            controller.setContext("", "");
            for (const char key : prefix)
                ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
            const auto index = rank(controller.page(), word);
            ASSERT_EQ(index, 2u);
            EXPECT_EQ(controller.page().items[index].fullPinyin, word);
            EXPECT_EQ(controller.page().items[index].consumedInputBytes, prefix.size());
            ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Digit, 0, '3'}));
            EXPECT_EQ(host.committed, word);
            EXPECT_TRUE(controller.page().rawInput.empty());
            ASSERT_TRUE(reopened.flush());
            const auto snapshot = reopened.sharedResources()->learning->snapshot();
            ASSERT_NE(snapshot->entry(word, word, {}, {}), nullptr);
            EXPECT_EQ(snapshot->entry(word, word, {}, {})->frequency, 2);
        }
    }
}

TEST_F(EnglishLearningAdmissionPersistence, FullRawCommitsAccumulateBeforePrefixCompletion) {
    for (const bool lmdb : {false, true}) {
        for (const auto &[word, prefix] : std::vector<std::pair<std::string, std::string>>{
                 {"sudoi", "sudo"}, {"gnueabihf", "gnue"}, {"microservices", "micro"}}) {
            SCOPED_TRACE(word + (lmdb ? " LMDB" : " SQLite"));
            pinyin::PinyinDataPaths paths;
            paths.learningStore = (root / (word + (lmdb ? ".lmdb" : ".sqlite3"))).string();
            paths.userDictionary = (root / "empty-user.dict").string();
            paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
            paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
            {
                pinyin::PinyinCandidateProvider provider(paths);
                AdmissionHost host;
                fcitx5::ModernIMEController controller(host, &provider);
                for (int count = 1; count <= 2; ++count) {
                    controller.setContext("", "");
                    for (const char key : word)
                        ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
                    ASSERT_EQ(controller.page().rawInput, word);
                    ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Enter}));
                    ASSERT_EQ(host.committed, word);
                    ASSERT_TRUE(provider.flush());
                    const auto snapshot = provider.sharedResources()->learning->snapshot();
                    ASSERT_NE(snapshot->entry(word, word, {}, {}), nullptr);
                    EXPECT_EQ(snapshot->entry(word, word, {}, {})->frequency, count);
                    ASSERT_TRUE(provider.replaceInput(prefix));
                    const auto index = rank(provider.page(), word);
                    if (count == 1) {
                        EXPECT_EQ(index, provider.page().items.size());
                    } else {
                        ASSERT_LT(index, provider.page().items.size());
                        EXPECT_EQ(provider.page().items[index].consumedInputBytes, prefix.size());
                        EXPECT_EQ(provider.page().items[index].fullPinyin, word);
                    }
                    controller.reset();
                }
            }
            pinyin::PinyinCandidateProvider reopened(paths);
            AdmissionHost host;
            fcitx5::ModernIMEController controller(host, &reopened);
            for (const char key : prefix)
                ASSERT_TRUE(controller.handle({fcitx5::KeyKind::Character, key}));
            const auto index = rank(controller.page(), word);
            ASSERT_LT(index, controller.page().items.size());
            EXPECT_EQ(controller.page().items[index].consumedInputBytes, prefix.size());
            ASSERT_TRUE(controller.select(index));
            EXPECT_EQ(host.committed, word);
            EXPECT_TRUE(controller.page().rawInput.empty());
        }
    }
}
} // namespace
