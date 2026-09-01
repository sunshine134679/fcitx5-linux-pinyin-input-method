#include "modernime/core/learning_store.h"

#include <sqlite3.h>

#include <filesystem>
#include <utility>

namespace modernime::core {
namespace {

constexpr const char *schema =
    "CREATE TABLE IF NOT EXISTS learning_entries ("
    "pinyin TEXT NOT NULL, phrase TEXT NOT NULL, "
    "context_before TEXT NOT NULL DEFAULT '', "
    "context_after TEXT NOT NULL DEFAULT '', "
    "frequency INTEGER NOT NULL DEFAULT 0, "
    "last_selected_ms INTEGER NOT NULL DEFAULT 0, "
    "negative_feedback INTEGER NOT NULL DEFAULT 0, "
    "suppressed INTEGER NOT NULL DEFAULT 0, "
    "PRIMARY KEY (pinyin, phrase, context_before, context_after));";

bool bindText(sqlite3_stmt *statement, int index, std::string_view value) {
    const char empty[] = "";
    const auto *data = value.data() != nullptr ? value.data() : empty;
    return sqlite3_bind_text(statement, index, data,
                             static_cast<int>(value.size()),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

} // namespace

LearningStore::LearningStore(std::filesystem::path path)
    : path_(std::move(path)) {}

LearningStore::~LearningStore() { close(); }

bool LearningStore::open() {
    if (db_ != nullptr) {
        return true;
    }
    if (path_.empty()) {
        return false;
    }
    std::error_code error;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            return false;
        }
    }
    if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
        close();
        return false;
    }
    // The settings client reads the same database while the input method
    // writes it; wait briefly for the other writer instead of failing with
    // SQLITE_BUSY.
    sqlite3_busy_timeout(db_, 1000);
    if (!execute("PRAGMA journal_mode=WAL;") || !execute(schema) ||
        !ensureSuppressionColumn()) {
        close();
        return false;
    }
    return true;
}

bool LearningStore::ensureSuppressionColumn() const {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql = "PRAGMA table_info(learning_entries);";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    bool hasColumn = false;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto *name = sqlite3_column_text(statement, 1);
        if (name != nullptr &&
            std::string_view(reinterpret_cast<const char *>(name)) ==
                "suppressed") {
            hasColumn = true;
            break;
        }
    }
    sqlite3_finalize(statement);
    if (stepResult != SQLITE_ROW && stepResult != SQLITE_DONE) {
        return false;
    }
    return hasColumn || execute(
                           "ALTER TABLE learning_entries ADD COLUMN "
                           "suppressed INTEGER NOT NULL DEFAULT 0;");
}

void LearningStore::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool LearningStore::execute(const char *sql) const {
    if (db_ == nullptr) {
        return false;
    }
    char *error = nullptr;
    const bool success = sqlite3_exec(db_, sql, nullptr, nullptr, &error) ==
                         SQLITE_OK;
    sqlite3_free(error);
    return success;
}

bool LearningStore::backupTo(const std::filesystem::path &path) const {
    if (db_ == nullptr || path.empty() || path == path_) {
        return false;
    }
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
    }

    sqlite3 *backupDatabase = nullptr;
    if (sqlite3_open_v2(path.c_str(), &backupDatabase,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        if (backupDatabase != nullptr) {
            sqlite3_close(backupDatabase);
        }
        return false;
    }
    sqlite3_busy_timeout(backupDatabase, 1000);
    auto *backup = sqlite3_backup_init(backupDatabase, "main", db_, "main");
    if (backup == nullptr) {
        sqlite3_close(backupDatabase);
        return false;
    }
    int result = SQLITE_OK;
    do {
        result = sqlite3_backup_step(backup, -1);
    } while (result == SQLITE_BUSY || result == SQLITE_LOCKED);
    const auto finishResult = sqlite3_backup_finish(backup);
    const bool success = result == SQLITE_DONE && finishResult == SQLITE_OK;
    sqlite3_close(backupDatabase);
    return success;
}

bool LearningStore::clear() {
    if (db_ == nullptr || !execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    if (!execute("DELETE FROM learning_entries;")) {
        execute("ROLLBACK;");
        return false;
    }
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        return false;
    }
    return true;
}

bool LearningStore::applySelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) const {
    if (db_ == nullptr) {
        return false;
    }
    const auto normalized = normalizePinyin(pinyin);
    constexpr const char *clearSuppressionSql =
        "UPDATE learning_entries SET suppressed = 0 "
        "WHERE pinyin = ? AND phrase = ?;";
    sqlite3_stmt *clearStatement = nullptr;
    if (sqlite3_prepare_v2(db_, clearSuppressionSql, -1, &clearStatement,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    const bool clearBound = bindText(clearStatement, 1, normalized) &&
                            bindText(clearStatement, 2, phrase);
    const bool clearSuccess = clearBound &&
                              sqlite3_step(clearStatement) == SQLITE_DONE;
    sqlite3_finalize(clearStatement);
    if (!clearSuccess) {
        return false;
    }
    constexpr const char *sql =
        "INSERT INTO learning_entries "
        "(pinyin, phrase, context_before, context_after, frequency, "
        "last_selected_ms) VALUES (?, ?, ?, ?, 1, ?) "
        "ON CONFLICT(pinyin, phrase, context_before, context_after) DO "
        "UPDATE SET frequency = CASE "
        "WHEN frequency >= 9223372036854775807 THEN 9223372036854775807 "
        "WHEN frequency < 0 THEN 1 ELSE frequency + 1 END, "
        "last_selected_ms = excluded.last_selected_ms, suppressed = 0;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const bool bound = bindText(statement, 1, normalized) &&
                       bindText(statement, 2, phrase) &&
                       bindText(statement, 3, contextBefore) &&
                       bindText(statement, 4, contextAfter) &&
                       sqlite3_bind_int64(statement, 5, nowMs) == SQLITE_OK;
    const bool success = bound && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return success;
}

bool LearningStore::recordSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    if (!applySelection(phrase, pinyin, contextBefore, contextAfter, nowMs)) {
        return false;
    }
    return pruneContextVariants() && pruneTotalEntries();
}

bool LearningStore::applyNegativeFeedback(std::string_view phrase,
                                          std::string_view pinyin) const {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql =
        "INSERT INTO learning_entries "
        "(pinyin, phrase, negative_feedback) VALUES (?, ?, 1) "
        "ON CONFLICT(pinyin, phrase, context_before, context_after) DO "
        "UPDATE SET negative_feedback = CASE "
        "WHEN negative_feedback >= 9223372036854775807 "
        "THEN 9223372036854775807 "
        "WHEN negative_feedback < 0 THEN 1 "
        "ELSE negative_feedback + 1 END;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto normalized = normalizePinyin(pinyin);
    const bool bound = bindText(statement, 1, normalized) &&
                       bindText(statement, 2, phrase);
    const bool success = bound && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return success;
}

bool LearningStore::recordNegativeFeedback(std::string_view phrase,
                                           std::string_view pinyin) {
    if (!applyNegativeFeedback(phrase, pinyin)) {
        return false;
    }
    return pruneTotalEntries();
}

bool LearningStore::applySuppression(std::string_view phrase,
                                     std::string_view pinyin) const {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql =
        "INSERT INTO learning_entries "
        "(pinyin, phrase, negative_feedback, suppressed) VALUES (?, ?, 1, 1) "
        "ON CONFLICT(pinyin, phrase, context_before, context_after) DO "
        "UPDATE SET negative_feedback = CASE "
        "WHEN negative_feedback >= 9223372036854775807 "
        "THEN 9223372036854775807 "
        "WHEN negative_feedback < 0 THEN 1 ELSE negative_feedback + 1 END, "
        "suppressed = 1;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto normalized = normalizePinyin(pinyin);
    const bool bound = bindText(statement, 1, normalized) &&
                       bindText(statement, 2, phrase);
    const bool success = bound && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return success;
}

bool LearningStore::recordSuppression(std::string_view phrase,
                                      std::string_view pinyin) {
    if (!applySuppression(phrase, pinyin)) {
        return false;
    }
    return pruneTotalEntries();
}

bool LearningStore::pruneContextVariants() const {
    constexpr const char *sql =
        "DELETE FROM learning_entries WHERE rowid IN ("
        "SELECT rowid FROM ("
        "SELECT rowid, ROW_NUMBER() OVER ("
        "PARTITION BY pinyin, phrase ORDER BY suppressed DESC, "
        "frequency DESC, last_selected_ms DESC, rowid DESC"
        ") AS variant_rank FROM learning_entries WHERE "
        "context_before <> '' OR context_after <> ''"
        ") WHERE variant_rank > 8);";
    return execute(sql);
}

bool LearningStore::pruneTotalEntries() const {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql =
        "DELETE FROM learning_entries WHERE rowid IN ("
        "SELECT rowid FROM ("
        "SELECT rowid, ROW_NUMBER() OVER ("
        "ORDER BY suppressed ASC, last_selected_ms DESC, frequency DESC, "
        "rowid ASC"
        ") AS keep_rank FROM learning_entries"
        ") WHERE keep_rank > ?);";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const bool bound = sqlite3_bind_int64(
                           statement, 1,
                           static_cast<sqlite3_int64>(totalEntryLimit_)) ==
                       SQLITE_OK;
    const bool success = bound && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return success;
}

bool LearningStore::recordBatch(const std::vector<LearningEvent> &events) {
    if (db_ == nullptr) {
        return false;
    }
    if (events.empty()) {
        return true;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    for (const auto &event : events) {
        bool success = false;
        switch (event.kind) {
        case LearningEvent::Kind::Selection:
            success = applySelection(event.phrase, event.pinyin,
                                     event.contextBefore, event.contextAfter,
                                     event.nowMs);
            break;
        case LearningEvent::Kind::NegativeFeedback:
            success = applyNegativeFeedback(event.phrase, event.pinyin);
            break;
        case LearningEvent::Kind::Suppression:
            success = applySuppression(event.phrase, event.pinyin);
            break;
        }
        if (!success) {
            execute("ROLLBACK;");
            return false;
        }
    }
    // Keep housekeeping in the same transaction as the events. Otherwise a
    // pruning failure can be reported after COMMIT, and LearningWriter's safe
    // retry would apply already-committed selections a second time.
    if (!pruneContextVariants() || !pruneTotalEntries()) {
        execute("ROLLBACK;");
        return false;
    }
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        return false;
    }
    return true;
}

std::shared_ptr<const LearningSnapshot> LearningStore::snapshot(
    std::int64_t) const {
    std::vector<LearningEntry> entries;
    if (db_ == nullptr) {
        return std::make_shared<const LearningSnapshot>(std::move(entries));
    }
    constexpr const char *sql =
        "SELECT phrase, pinyin, context_before, context_after, frequency, "
        "last_selected_ms, negative_feedback, suppressed "
        "FROM learning_entries;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return {};
    }
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto textAt = [statement](int index) {
            const auto *value = sqlite3_column_text(statement, index);
            return value != nullptr
                       ? std::string(reinterpret_cast<const char *>(value))
                       : std::string();
        };
        entries.push_back({textAt(0), textAt(1), textAt(2), textAt(3),
                           sqlite3_column_int64(statement, 4),
                           sqlite3_column_int64(statement, 5),
                           sqlite3_column_int64(statement, 6),
                           sqlite3_column_int(statement, 7) != 0});
    }
    sqlite3_finalize(statement);
    if (stepResult != SQLITE_DONE) {
        return {};
    }
    return std::make_shared<const LearningSnapshot>(std::move(entries));
}

} // namespace modernime::core
