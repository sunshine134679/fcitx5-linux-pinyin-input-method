#include "modernime/core/learning_store.h"
#include "modernime/core/personal_engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
using modernime::core::LearningEvent;
using modernime::core::LearningStore;
using modernime::core::PersonalEngine;

class PersonalizationStorageRegression : public testing::Test {
protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "modernime-storage-regression-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root_ = pattern;
    }
    void TearDown() override { std::filesystem::remove_all(root_); }
    std::filesystem::path path() const { return root_ / "learning.lmdb"; }
    std::filesystem::path root_;
};

LearningEvent selection(std::string phrase, std::string pinyin) {
    LearningEvent event;
    event.phrase = std::move(phrase);
    event.pinyin = std::move(pinyin);
    event.nowMs = 1'000'000;
    return event;
}

TEST_F(PersonalizationStorageRegression, FailedBatchNeverCommitsAnEarlierSelection) {
    LearningStore store(path());
    ASSERT_TRUE(store.open());
    const std::vector<LearningEvent> batch{
        selection("你好", "nihao"), selection(std::string(600, 'x'), "x")};
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_FALSE(store.recordBatch(batch));
        EXPECT_TRUE(store.personalEngine()->ExportLearningEntries().empty());
        EXPECT_TRUE(store.personalEngine()->QueryLmdbCandidates("nihao").empty());
        ASSERT_TRUE(store.personalEngine()->ReloadFromLmdb());
        EXPECT_EQ(store.personalEngine()->GetTotalEntries(), 0u);
    }
    ASSERT_TRUE(store.recordBatch({selection("你好", "nihao")}));
    store.close();
    ASSERT_TRUE(store.open());
    const auto candidates = store.personalEngine()->QueryLmdbCandidates("nihao");
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].meta.hit_count, 1u);
}

TEST_F(PersonalizationStorageRegression, ContextVariantPruningSurvivesEveryReload) {
    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(path()));
    ASSERT_TRUE(engine.RecordUsage("nihao", "你好"));
    for (int i = 0; i < 24; ++i) {
        ASSERT_TRUE(engine.RecordUsage("nihao", "你好", 1.0f,
                                       "context" + std::to_string(i), {},
                                       1'000'000 + i));
        EXPECT_LE(engine.GetTotalEntries(), 9u); // generic + eight contexts
        ASSERT_TRUE(engine.ReloadFromLmdb());
        EXPECT_LE(engine.GetTotalEntries(), 9u);
    }
    engine.CloseLmdb();
    ASSERT_TRUE(engine.OpenLmdb(path()));
    EXPECT_EQ(engine.GetTotalEntries(), 9u);
}

TEST_F(PersonalizationStorageRegression, GenericSuppressionBlocksPrefixContextVariants) {
    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(path()));
    ASSERT_TRUE(engine.RecordUsage("nihao", "你好", 1.0f, "前文"));
    ASSERT_TRUE(engine.RecordSuppression("nihao", "你好"));
    EXPECT_TRUE(engine.QueryLmdbCandidates("nihao").empty());
    EXPECT_TRUE(engine.QueryLmdbPrefixCandidates("ni").empty());
    engine.CloseLmdb();
    ASSERT_TRUE(engine.OpenLmdb(path()));
    EXPECT_TRUE(engine.QueryLmdbPrefixCandidates("ni").empty());
    ASSERT_TRUE(engine.RecordUsage("nihao", "你好", 1.0f, "新前文"));
    EXPECT_EQ(engine.QueryLmdbPrefixCandidates("ni").size(), 1u);
}
TEST_F(PersonalizationStorageRegression, ConsecutiveClearsInvalidateEveryOlderBatch) {
    LearningStore writer(path());
    LearningStore settings(path());
    ASSERT_TRUE(writer.open());
    ASSERT_TRUE(settings.open());
    ASSERT_TRUE(writer.clear());
    auto stale = selection("你好", "nihao");
    stale.resetRevision = writer.resetRevision();
    ASSERT_TRUE(settings.clear()); // settings still holds the pre-first-clear cache
    ASSERT_TRUE(writer.recordBatch({stale}));
    EXPECT_TRUE(writer.personalEngine()->QueryLmdbCandidates("nihao").empty());
    EXPECT_EQ(writer.resetRevision(), 2);
}

TEST_F(PersonalizationStorageRegression, OldOversizedContextGroupsAreRepairedOnOpen) {
    PersonalEngine engine;
    ASSERT_TRUE(engine.OpenLmdb(path()));
    std::vector<modernime::core::LearningEntry> entries;
    for (int i = 0; i < 12; ++i) {
        modernime::core::LearningEntry entry;
        entry.phrase = "你好";
        entry.pinyin = "nihao";
        entry.contextBefore = "context" + std::to_string(i);
        entry.frequency = 1;
        entry.lastSelectedMs = 1'000'000 + i;
        entries.push_back(std::move(entry));
    }
    ASSERT_TRUE(engine.ApplyLearningBatch(entries, true));
    ASSERT_EQ(engine.GetTotalEntries(), 12u);
    engine.CloseLmdb();
    ASSERT_TRUE(engine.OpenLmdb(path()));
    EXPECT_EQ(engine.GetTotalEntries(), 8u);
    ASSERT_TRUE(engine.ReloadFromLmdb());
    EXPECT_EQ(engine.GetTotalEntries(), 8u);
    const auto retained = engine.ExportLearningEntries();
    for (const auto &entry : retained) {
        EXPECT_GE(entry.lastSelectedMs, 1'000'004);
    }
}

TEST_F(PersonalizationStorageRegression, BatchMapGrowthAppliesEachEventOnlyOnce) {
    modernime::core::EngineConfig config;
    config.initial_lmdb_map_size = 64 * 1024;
    PersonalEngine engine(config);
    ASSERT_TRUE(engine.OpenLmdb(path()));
    std::vector<LearningEvent> batch;
    for (int i = 0; i < 1500; ++i) {
        batch.push_back(selection("扩容词" + std::to_string(i),
                                  "!u!test," + std::to_string(i)));
    }
    ASSERT_TRUE(engine.RecordLearningBatch(batch));
    EXPECT_GT(engine.LmdbMapSize(), 64u * 1024u);
    ASSERT_TRUE(engine.ReloadFromLmdb());
    const auto entries = engine.ExportLearningEntries();
    ASSERT_EQ(entries.size(), 1500u);
    for (const auto &entry : entries) {
        EXPECT_EQ(entry.frequency, 1);
    }
}

TEST_F(PersonalizationStorageRegression, FailedBatchPreservesExistingFeedbackAndSuppression) {
    LearningStore store(path());
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(store.recordSelection("你好", "nihao", "前文", "", 1'000'000));
    ASSERT_TRUE(store.recordSuppression("你好", "nihao"));
    auto feedback = selection("你好", "nihao");
    feedback.kind = LearningEvent::Kind::NegativeFeedback;
    auto relearn = selection("你好", "nihao");
    relearn.contextBefore = "新前文";
    // Empty phrases fail after staging the earlier feedback and selection;
    // unlike oversized DUPSORT values they pass the record-size preflight.
    EXPECT_FALSE(store.recordBatch({feedback, relearn, selection("", "x")}));
    EXPECT_TRUE(store.personalEngine()->QueryCandidates("nihao").empty());
    EXPECT_TRUE(store.personalEngine()->QueryLmdbPrefixCandidates("ni").empty());
    ASSERT_TRUE(store.personalEngine()->ReloadFromLmdb());
    const auto entries = store.personalEngine()->ExportLearningEntries();
    ASSERT_EQ(entries.size(), 2u);
    int negative = 0;
    int frequency = 0;
    for (const auto &entry : entries) {
        negative += static_cast<int>(entry.negativeFeedback);
        frequency += static_cast<int>(entry.frequency);
    }
    EXPECT_EQ(negative, 1);
    EXPECT_EQ(frequency, 1);
}
TEST_F(PersonalizationStorageRegression, CapacityPruningCommitsWithTheWholeBatch) {
    modernime::core::EngineConfig config;
    config.max_dict_capacity = 8;
    config.prune_target_size = 6;
    PersonalEngine engine(config);
    ASSERT_TRUE(engine.OpenLmdb(path()));
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(engine.RecordUsage("!u!key," + std::to_string(i),
                                       "词" + std::to_string(i)));
    }
    auto good = selection("新词", "xinci");
    good.nowMs = 2'000'000;
    auto bad = selection(std::string(600, 'x'), "x");
    bad.nowMs = 3'000'000;
    EXPECT_FALSE(engine.RecordLearningBatch({good, bad}));
    EXPECT_EQ(engine.GetTotalEntries(), 8u);
    ASSERT_TRUE(engine.ReloadFromLmdb());
    EXPECT_EQ(engine.GetTotalEntries(), 8u);
    ASSERT_TRUE(engine.RecordLearningBatch({good}));
    EXPECT_EQ(engine.GetTotalEntries(), 6u);
    ASSERT_TRUE(engine.ReloadFromLmdb());
    EXPECT_EQ(engine.GetTotalEntries(), 6u);
}
TEST_F(PersonalizationStorageRegression, CanonicalEnglishSelectionUnsuppressesLegacyAliases) {
    for (const auto *extension : {".sqlite3", ".lmdb"}) {
        SCOPED_TRACE(extension);
        LearningStore store(root_ / (std::string("english") + extension));
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.recordSelection("garage", "GARA", "", "", 1'000'000));
        ASSERT_TRUE(store.recordSuppression("garage", "gara"));
        ASSERT_TRUE(store.recordSuppression("garage", "unrelated"));
        ASSERT_TRUE(store.snapshot()->isSuppressed("garage", "garage"));
        ASSERT_TRUE(store.recordBatch({selection("garage", "GARAGE")}));
        store.close();
        ASSERT_TRUE(store.open());
        const auto snapshot = store.snapshot();
        EXPECT_FALSE(snapshot->isSuppressed("garage", "garage"));
        EXPECT_TRUE(snapshot->isSuppressed("garage", "unrelated"));
        const auto words = snapshot->recallEnglishWords("gar", 10, 1'000'000);
        EXPECT_EQ(words.size(), 1u);
        if (words.size() != 1) {
            continue;
        }
        EXPECT_EQ(words[0].text, "garage");
        EXPECT_EQ(words[0].frequency, 2);
        ASSERT_TRUE(store.recordSuppression("garage", "GAR"));
        ASSERT_TRUE(store.recordSelection("garage", "garage", "", "", 1'000'001));
        store.close();
        ASSERT_TRUE(store.open());
        EXPECT_FALSE(store.snapshot()->isSuppressed("garage", "garage"));
        const auto directWords = store.snapshot()->recallEnglishWords("gar", 10, 1'000'001);
        ASSERT_EQ(directWords.size(), 1u);
        EXPECT_EQ(directWords[0].frequency, 3);
    }
}

TEST_F(PersonalizationStorageRegression, NonEnglishSelectionDoesNotUnsuppressPrefixMappings) {
    for (const auto *extension : {".sqlite3", ".lmdb"}) {
        SCOPED_TRACE(extension);
        LearningStore store(root_ / (std::string("mapped") + extension));
        ASSERT_TRUE(store.open());
        for (const auto *phrase : {"你好", "term1", "___"}) {
            ASSERT_TRUE(store.recordSuppression(phrase, "te"));
            ASSERT_TRUE(store.recordSelection(phrase, phrase, "", "", 1'000'000));
            EXPECT_TRUE(store.snapshot()->isSuppressed(phrase, "te"));
        }
    }
}

TEST_F(PersonalizationStorageRegression, LmdbReadStateTagsEntriesWithTheirOwnResetRevision) {
    LearningStore reader(path());
    LearningStore writer(path());
    ASSERT_TRUE(reader.open());
    ASSERT_TRUE(writer.open());
    std::atomic<bool> finished = false;
    std::atomic<int> write_errors = 0;
    std::thread clearing([&] {
        for (int revision = 1; revision <= 1500; ++revision) {
            if (!writer.clear() ||
                !writer.recordSelection("generation" + std::to_string(revision),
                                        "generation", "", "", 1'000'000)) {
                ++write_errors;
                break;
            }
        }
        finished = true;
    });
    int mismatches = 0;
    int read_errors = 0;
    do {
        const auto state = reader.readState();
        if (!state) {
            ++read_errors;
            continue;
        }
        for (const auto &entry : state->snapshot->entries()) {
            if (entry.phrase != "generation" + std::to_string(state->resetRevision)) {
                ++mismatches;
            }
        }
    } while (!finished);
    clearing.join();
    EXPECT_EQ(write_errors, 0);
    EXPECT_EQ(read_errors, 0);
    EXPECT_EQ(mismatches, 0);
}
} // namespace
