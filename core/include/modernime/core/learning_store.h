#pragma once

#include "modernime/core/commit_event.h"
#include "modernime/core/learning_event.h"
#include "modernime/core/learning_policy.h"
#include "modernime/core/learning_snapshot.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace modernime::core {

namespace detail {
// Returns the lowercase ASCII word only for an explicit full-word selection.
inline std::string canonicalEnglishSelectionWord(std::string_view phrase,
                                                 std::string_view pinyin) {
    return ::modernime::core::canonicalEnglishSelectionWord(phrase, pinyin);
}
} // namespace detail

struct LearningStoreState final {
    std::shared_ptr<const LearningSnapshot> snapshot;
    std::int64_t resetRevision = 0;
};

class PersonalEngine;

class LearningStore final {
public:
    explicit LearningStore(std::filesystem::path path);
    ~LearningStore();

    LearningStore(const LearningStore &) = delete;
    LearningStore &operator=(const LearningStore &) = delete;

    bool open();
    // Opens an existing store without schema writes or SQLite busy waits.
    bool openReadOnly();
    void close();
    bool isLmdbBackend() const noexcept { return useLmdb_; }
    bool migrateFromSqlite(const std::filesystem::path &sqlitePath);
    PersonalEngine *personalEngine() noexcept { return lmdbEngine_.get(); }
    const PersonalEngine *personalEngine() const noexcept {
        return lmdbEngine_.get();
    }
    // Overrides the retained-entry cap; zero restores the default.
    void setTotalEntryLimit(std::size_t limit);
    bool recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    bool recordCommit(const CommitEvent &event);
    bool recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    bool recordSuppression(std::string_view phrase,
                           std::string_view pinyin);
    bool recordBatch(const std::vector<LearningEvent> &events,
                     bool *pruned = nullptr,
                     bool *external_changed = nullptr);
    bool backupTo(const std::filesystem::path &path) const;
    bool clear();
    std::optional<std::int64_t> resetRevision() const;
    int schemaVersion() const;
    std::uint64_t dataVersion() const;
    // Reads the reset marker and entries from the same read transaction.
    std::optional<LearningStoreState> readState() const;
    std::optional<std::size_t> entryCount() const;
    // Self-verifies database structure, schema, metadata, and data readability.
    bool verifySelf() const;
    std::shared_ptr<const LearningSnapshot> snapshot(std::int64_t nowMs = 0) const;

    enum class FaultInjection {
        None,
        FailBeforeCommit,
        FailDuringPrune
    };
    void setFaultInjection(FaultInjection fi) noexcept { faultInjection_ = fi; }

private:
    bool execute(const char *sql) const;
    bool ensureSuppressionColumn() const;
    bool pruneContextVariants(bool *pruned = nullptr) const;
    bool pruneTotalEntries(bool *pruned = nullptr) const;
    bool applySelection(std::string_view phrase, std::string_view pinyin,
                        std::string_view contextBefore,
                        std::string_view contextAfter, std::int64_t nowMs) const;
    bool applyNegativeFeedback(std::string_view phrase,
                               std::string_view pinyin) const;
    bool applySuppression(std::string_view phrase,
                          std::string_view pinyin) const;

    std::filesystem::path path_;
    sqlite3 *db_ = nullptr;
    std::unique_ptr<PersonalEngine> lmdbEngine_;
    bool useLmdb_ = false;
    std::size_t totalEntryLimit_ = kMaxLearningEntries;
    mutable std::uint64_t lastDataVersion_ = 0;
    FaultInjection faultInjection_ = FaultInjection::None;
};

} // namespace modernime::core
