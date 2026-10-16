#include <gtest/gtest.h>

#include "modernime/core/commit_event.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_event.h"
#include "modernime/core/learning_policy.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/personal_engine.h"
#include "modernime/core/pinyin_match.h"

#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace modernime::core {
namespace {

class TempDirectoryFixture {
public:
    TempDirectoryFixture() {
        char tempPath[] = "/tmp/modernime_learning_semantics_test_XXXXXX";
        const char *created = mkdtemp(tempPath);
        if (created != nullptr) {
            path_ = created;
        } else {
            path_ = "/tmp/modernime_learning_semantics_fallback";
            std::filesystem::create_directories(path_);
        }
    }

    ~TempDirectoryFixture() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

TEST(ArchitectureLearningSemanticsRegression, DecideCommitLearningPolicy) {
    // 1. Command role must be ignored
    CommitEvent cmdEvent;
    cmdEvent.text = "剪贴板内容";
    cmdEvent.role = CommitCandidateRole::Command;
    EXPECT_EQ(decideCommitLearning(cmdEvent, false), CommitLearningDecision::IgnoreCommand);
    EXPECT_EQ(decideCommitLearning(cmdEvent, true), CommitLearningDecision::IgnoreCommand);

    // 2. Raw pinyin sequence (e.g. "jianpan") not in EnglishDictionary must be ignored
    CommitEvent rawPinyinEvent;
    rawPinyinEvent.text = "jianpan";
    rawPinyinEvent.role = CommitCandidateRole::Raw;
    EXPECT_EQ(decideCommitLearning(rawPinyinEvent, false), CommitLearningDecision::IgnoreRawPinyin);
    EXPECT_EQ(decideCommitLearning(rawPinyinEvent, true), CommitLearningDecision::IgnoreRawPinyin);

    // 3. Raw English word (e.g. "in", "it") in EnglishDictionary must be recorded
    CommitEvent rawEnglishEvent;
    rawEnglishEvent.text = "in";
    rawEnglishEvent.role = CommitCandidateRole::Raw;
    EXPECT_EQ(decideCommitLearning(rawEnglishEvent, false), CommitLearningDecision::Record);

    // 4. Raw non-pinyin (e.g. "xyz123") must be recorded
    CommitEvent rawCodeEvent;
    rawCodeEvent.text = "xyz123";
    rawCodeEvent.role = CommitCandidateRole::Raw;
    EXPECT_EQ(decideCommitLearning(rawCodeEvent, false), CommitLearningDecision::Record);

    // 5. Protected core Chinese default words
    const std::vector<std::pair<std::string, std::string>> coreWords = {
        {"是", "shi"}, {"的", "de"}, {"你", "ni"}, {"好", "hao"},
        {"吗", "ma"},  {"和", "he"}, {"在", "zai"}, {"有", "you"},
        {"了", "le"},  {"不", "bu"}, {"我", "wo"}, {"这个", "zhege"},
        {"不是", "bushi"}, {"好的", "haode"}, {"可以", "keyi"}
    };

    for (const auto &[word, py] : coreWords) {
        EXPECT_TRUE(isProtectedCoreDefault(word, py)) << "Expected core default for " << word;
        EXPECT_TRUE(PinyinMatchPolicy::isCoreChineseWord(word, py)) << "Delegation failed for " << word;

        CommitEvent coreEvent;
        coreEvent.text = word;
        coreEvent.fullPinyin = py;
        coreEvent.role = CommitCandidateRole::Chinese;
        coreEvent.candidateIndex = 0;
        coreEvent.isDeviation = false;
        coreEvent.isPartial = false;

        // Without prior positive frequency, core default is ignored
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::IgnoreCoreDefault)
            << "Failed for " << word;

        // With prior positive frequency, core default is recorded
        EXPECT_EQ(decideCommitLearning(coreEvent, true), CommitLearningDecision::Record)
            << "Failed for " << word;

        // Core default with candidateIndex >= 1 is recorded even without prior frequency
        coreEvent.candidateIndex = 1;
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::Record);
        coreEvent.candidateIndex = 0;

        // Core default with isDeviation is recorded
        coreEvent.isDeviation = true;
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::Record);
        coreEvent.isDeviation = false;

        // Core default with isPartial is recorded
        coreEvent.isPartial = true;
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::Record);
        coreEvent.isPartial = false;

        // Core default with surrounding context is recorded
        coreEvent.contextBefore = "前文";
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::Record);
        coreEvent.contextBefore.clear();
        coreEvent.contextAfter = "后文";
        EXPECT_EQ(decideCommitLearning(coreEvent, false), CommitLearningDecision::Record);
    }

    // 6. Non-core Chinese words are recorded unconditionally at rank 0
    CommitEvent regularEvent;
    regularEvent.text = "北京";
    regularEvent.fullPinyin = "beijing";
    regularEvent.role = CommitCandidateRole::Chinese;
    regularEvent.candidateIndex = 0;
    EXPECT_FALSE(isProtectedCoreDefault(regularEvent.text, regularEvent.fullPinyin));
    EXPECT_EQ(decideCommitLearning(regularEvent, false), CommitLearningDecision::Record);
    EXPECT_EQ(decideCommitLearning(regularEvent, true), CommitLearningDecision::Record);
}

TEST(ArchitectureLearningSemanticsRegression, CanonicalEnglishSelectionWord) {
    // Valid lowercase, uppercase, and mixed-case
    EXPECT_EQ(canonicalEnglishSelectionWord("hello", "hello"), "hello");
    EXPECT_EQ(canonicalEnglishSelectionWord("Hello", "hello"), "hello");
    EXPECT_EQ(canonicalEnglishSelectionWord("HELLO", "hello"), "hello");

    // Hyphen and underscore allowed
    EXPECT_EQ(canonicalEnglishSelectionWord("open-source", "open-source"), "open-source");
    EXPECT_EQ(canonicalEnglishSelectionWord("snake_case", "snake_case"), "snake_case");

    // Pinyin with syllable apostrophes matched after normalization
    EXPECT_EQ(canonicalEnglishSelectionWord("hello", "hel'lo"), "hello");

    // Rejection cases
    EXPECT_EQ(canonicalEnglishSelectionWord("hello", "world"), ""); // mismatch
    EXPECT_EQ(canonicalEnglishSelectionWord("hello123", "hello123"), ""); // digits rejected
    EXPECT_EQ(canonicalEnglishSelectionWord("hello world", "hello world"), ""); // spaces rejected
    EXPECT_EQ(canonicalEnglishSelectionWord("", ""), ""); // empty rejected

    std::string longWord(65, 'a');
    EXPECT_EQ(canonicalEnglishSelectionWord(longWord, longWord), ""); // > 64 chars rejected
}

TEST(ArchitectureLearningSemanticsRegression, ThreeWayReplayConsistency) {
    TempDirectoryFixture fixture;
    const auto sqlitePath = fixture.path() / "test_store.sqlite3";
    const auto lmdbPath = fixture.path() / "test_store.lmdb";

    // 1. Initialize SQLite store
    LearningStore sqliteStore(sqlitePath);
    ASSERT_TRUE(sqliteStore.open());

    // 2. Initialize LMDB PersonalEngine
    EngineConfig cfg;
    cfg.max_dict_capacity = 200000;
    cfg.prune_target_size = 200000;
    PersonalEngine lmdbEngine(cfg);
    ASSERT_TRUE(lmdbEngine.OpenLmdb(lmdbPath, false));

    // 3. Initialize In-Memory LearningSnapshot
    LearningSnapshot snapshot;

    // --- Step A: Commit non-core Chinese word "北京" ("beijing") ---
    CommitEvent evBeijing;
    evBeijing.text = "北京";
    evBeijing.fullPinyin = "beijing";
    evBeijing.role = CommitCandidateRole::Chinese;
    evBeijing.candidateIndex = 0;
    evBeijing.timestampMs = 1000;

    EXPECT_TRUE(sqliteStore.recordCommit(evBeijing));
    EXPECT_TRUE(lmdbEngine.RecordUsage("beijing", "北京", 1.0f, "", "", 1000));
    snapshot.recordSelection("北京", "beijing", "", "", 1000);

    // Verify presence and positive frequency across all 3
    EXPECT_TRUE(sqliteStore.snapshot()->hasPositiveFrequency("北京", "beijing"));
    EXPECT_TRUE(lmdbEngine.HasPositiveFrequency("beijing", "北京"));
    EXPECT_TRUE(snapshot.hasPositiveFrequency("北京", "beijing"));

    // --- Step B: Core word "你" ("ni") without positive history at rank 0 must be skipped ---
    CommitEvent evNi0;
    evNi0.text = "你";
    evNi0.fullPinyin = "ni";
    evNi0.role = CommitCandidateRole::Chinese;
    evNi0.candidateIndex = 0;
    evNi0.timestampMs = 2000;

    EXPECT_FALSE(sqliteStore.snapshot()->hasPositiveFrequency("你", "ni"));
    EXPECT_FALSE(lmdbEngine.HasPositiveFrequency("ni", "你"));
    EXPECT_FALSE(snapshot.hasPositiveFrequency("你", "ni"));

    // Both Writer and Store policy decide to ignore
    EXPECT_EQ(decideCommitLearning(evNi0, false), CommitLearningDecision::IgnoreCoreDefault);
    EXPECT_TRUE(sqliteStore.recordCommit(evNi0)); // returns true (safe skip)

    // Verify still not learned
    EXPECT_FALSE(sqliteStore.snapshot()->hasPositiveFrequency("你", "ni"));
    EXPECT_FALSE(lmdbEngine.HasPositiveFrequency("ni", "你"));
    EXPECT_FALSE(snapshot.hasPositiveFrequency("你", "ni"));

    // --- Step C: Core word "你" chosen as secondary candidate (rank 1) is learned ---
    CommitEvent evNi1;
    evNi1.text = "你";
    evNi1.fullPinyin = "ni";
    evNi1.role = CommitCandidateRole::Chinese;
    evNi1.candidateIndex = 1;
    evNi1.isDeviation = true;
    evNi1.timestampMs = 3000;

    EXPECT_EQ(decideCommitLearning(evNi1, false), CommitLearningDecision::Record);
    EXPECT_TRUE(sqliteStore.recordCommit(evNi1));
    EXPECT_TRUE(lmdbEngine.RecordUsage("ni", "你", 1.0f, "", "", 3000));
    snapshot.recordSelection("你", "ni", "", "", 3000);

    EXPECT_TRUE(sqliteStore.snapshot()->hasPositiveFrequency("你", "ni"));
    EXPECT_TRUE(lmdbEngine.HasPositiveFrequency("ni", "你"));
    EXPECT_TRUE(snapshot.hasPositiveFrequency("你", "ni"));

    // --- Step D: Subsequent rank 0 selection of "你" now recorded because history exists ---
    CommitEvent evNiAgain;
    evNiAgain.text = "你";
    evNiAgain.fullPinyin = "ni";
    evNiAgain.role = CommitCandidateRole::Chinese;
    evNiAgain.candidateIndex = 0;
    evNiAgain.timestampMs = 4000;

    EXPECT_EQ(decideCommitLearning(evNiAgain, true), CommitLearningDecision::Record);
    EXPECT_TRUE(sqliteStore.recordCommit(evNiAgain));
    EXPECT_TRUE(lmdbEngine.RecordUsage("ni", "你", 1.0f, "", "", 4000));
    snapshot.recordSelection("你", "ni", "", "", 4000);

    // --- Step E: Negative Feedback on "北京" ---
    EXPECT_TRUE(sqliteStore.recordNegativeFeedback("北京", "beijing"));
    EXPECT_TRUE(lmdbEngine.RecordNegativeFeedback("beijing", "北京"));
    snapshot.recordNegativeFeedback("北京", "beijing");

    // --- Step F: Suppression on "北京" ---
    EXPECT_TRUE(sqliteStore.recordSuppression("北京", "beijing"));
    EXPECT_TRUE(lmdbEngine.RecordSuppression("beijing", "北京"));
    snapshot.recordSuppression("北京", "beijing");

    EXPECT_TRUE(sqliteStore.snapshot()->isSuppressed("北京", "beijing"));
    EXPECT_FALSE(lmdbEngine.HasPositiveFrequency("beijing", "北京")); // suppressed returns false
    EXPECT_TRUE(snapshot.isSuppressed("北京", "beijing"));

    // --- Step G: Subsequent Selection unsuppresses "北京" ---
    EXPECT_TRUE(sqliteStore.recordSelection("北京", "beijing", "", "", 5000));
    EXPECT_TRUE(lmdbEngine.RecordUsage("beijing", "北京", 1.0f, "", "", 5000));
    snapshot.recordSelection("北京", "beijing", "", "", 5000);

    EXPECT_FALSE(sqliteStore.snapshot()->isSuppressed("北京", "beijing"));
    EXPECT_TRUE(sqliteStore.snapshot()->hasPositiveFrequency("北京", "beijing"));
    EXPECT_TRUE(lmdbEngine.HasPositiveFrequency("beijing", "北京"));
    EXPECT_FALSE(snapshot.isSuppressed("北京", "beijing"));

    // --- Step H: Clear / Reset increments revision and clears entries ---
    const auto oldRevisionSqlite = sqliteStore.resetRevision().value_or(0);
    const auto oldRevisionLmdb = lmdbEngine.ResetRevision();

    EXPECT_TRUE(sqliteStore.clear());
    EXPECT_TRUE(lmdbEngine.Clear());
    snapshot = LearningSnapshot();

    EXPECT_GT(sqliteStore.resetRevision().value_or(0), oldRevisionSqlite);
    EXPECT_GT(lmdbEngine.ResetRevision(), oldRevisionLmdb);

    EXPECT_FALSE(sqliteStore.snapshot()->hasPositiveFrequency("北京", "beijing"));
    EXPECT_FALSE(lmdbEngine.HasPositiveFrequency("beijing", "北京"));
    EXPECT_FALSE(snapshot.hasPositiveFrequency("北京", "beijing"));
}

TEST(ArchitectureLearningSemanticsRegression, SqliteSingleEventAtomicityAndFaultInjection) {
    TempDirectoryFixture fixture;
    const auto sqlitePath = fixture.path() / "fault_test.sqlite3";

    LearningStore store(sqlitePath);
    ASSERT_TRUE(store.open());

    auto countEntries = [&]() -> int {
        sqlite3 *rawDb = nullptr;
        if (sqlite3_open_v2(sqlitePath.c_str(), &rawDb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            return -1;
        }
        sqlite3_stmt *stmt = nullptr;
        int count = -1;
        if (sqlite3_prepare_v2(rawDb, "SELECT COUNT(*) FROM learning_entries;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(rawDb);
        return count;
    };

    EXPECT_EQ(countEntries(), 0);

    // 1. Fault injection before commit: must rollback completely
    store.setFaultInjection(LearningStore::FaultInjection::FailBeforeCommit);
    EXPECT_FALSE(store.recordSelection("原子性词", "yuanzixingci", "", "", 1000));
    EXPECT_EQ(countEntries(), 0) << "Fault injection before commit leaked partial entries!";

    // 2. Fault injection during prune: must rollback completely
    store.setFaultInjection(LearningStore::FaultInjection::FailDuringPrune);
    EXPECT_FALSE(store.recordSelection("原子性词", "yuanzixingci", "", "", 1000));
    EXPECT_EQ(countEntries(), 0) << "Fault injection during prune leaked partial entries!";

    // 3. Normal execution succeeds and writes exactly 1 entry atomically
    store.setFaultInjection(LearningStore::FaultInjection::None);
    EXPECT_TRUE(store.recordSelection("原子性词", "yuanzixingci", "", "", 1000));
    EXPECT_EQ(countEntries(), 1);

    // 4. Batch replay with stale revision is cleanly skipped
    const auto curRev = store.resetRevision().value_or(0);
    LearningEvent staleEvent;
    staleEvent.kind = LearningEvent::Kind::Selection;
    staleEvent.phrase = "旧世代词";
    staleEvent.pinyin = "jiushidaici";
    staleEvent.resetRevision = curRev - 1; // Stale revision!

    EXPECT_TRUE(store.recordBatch({staleEvent}));
    // Stale event must not have created any row
    EXPECT_EQ(countEntries(), 1);
}

TEST(ArchitectureLearningSemanticsRegression, SemanticDifferencesSpecification) {
    // Document and verify backend-specific invariants:
    // 1. Context sanitization window:
    // LMDB truncates surrounding context to 12 codepoints via SanitizeSurroundingContext
    std::string longContext = "12345678901234567890";
    std::string sanitized = PersonalEngine::SanitizeSurroundingContext(longContext, 12);
    EXPECT_LE(sanitized.size(), 12U);

    // 2. Pinyin key format:
    // LMDB uses !u! prefix and comma separation
    std::string normKey = PersonalEngine::NormalizePinyinKey("ni'hao");
    EXPECT_TRUE(normKey.starts_with("!u!"));
    EXPECT_EQ(normKey, "!u!ni,hao");

    // 3. Capacity limit default: 200,000 entries
    EXPECT_EQ(kMaxLearningEntries, 200000U);
}

} // namespace
} // namespace modernime::core
