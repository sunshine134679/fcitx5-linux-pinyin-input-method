#include "modernime/core/learning_store.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/personal_engine.h"
#include "modernime/core/pinyin_match.h"

#include <sqlite3.h>

#include <cstdlib>
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

bool shouldUseLmdb(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    if (ext == ".lmdb" || ext == ".mdb") {
        return true;
    }
    if (const char *env = std::getenv("MODERNIME_LEARNING_BACKEND")) {
        if (std::string_view(env) == "lmdb" && ext != ".sqlite3") {
            return true;
        }
    }
    return false;
}

} // namespace

LearningStore::LearningStore(std::filesystem::path path)
    : path_(std::move(path)),
      useLmdb_(shouldUseLmdb(path_)) {}

LearningStore::~LearningStore() { close(); }

void LearningStore::setTotalEntryLimit(std::size_t limit) {
    totalEntryLimit_ = limit == 0 ? kMaxLearningEntries : limit;
    if (lmdbEngine_ != nullptr) {
        const auto pruneTarget = (totalEntryLimit_ * 4) / 5;
        lmdbEngine_->SetMaxCapacity(totalEntryLimit_, pruneTarget);
    }
}

bool LearningStore::open() {
    if (useLmdb_) {
        if (lmdbEngine_ != nullptr && lmdbEngine_->IsLmdbOpen()) {
            return true;
        }
        if (path_.empty()) {
            return false;
        }
        EngineConfig cfg;
        cfg.max_dict_capacity = totalEntryLimit_;
        cfg.prune_target_size = (totalEntryLimit_ * 4) / 5;
        lmdbEngine_ = std::make_unique<PersonalEngine>(cfg);
        if (!lmdbEngine_->OpenLmdb(path_, false)) {
            lmdbEngine_.reset();
            return false;
        }
        return true;
    }

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
        !ensureSuppressionColumn() ||
        !execute("CREATE TABLE IF NOT EXISTS learning_state ("
                 "id INTEGER PRIMARY KEY CHECK (id = 1), "
                 "reset_revision INTEGER NOT NULL DEFAULT 0);"
                 "INSERT OR IGNORE INTO learning_state (id) VALUES (1);")) {
        close();
        return false;
    }
    const int currentVersion = schemaVersion();
    if (currentVersion == 0) {
        if (!execute("PRAGMA user_version = 1;")) {
            close();
            return false;
        }
    }
    return true;
}

bool LearningStore::openReadOnly() {
    if (useLmdb_) {
        if (lmdbEngine_ != nullptr && lmdbEngine_->IsLmdbOpen()) {
            return true;
        }
        if (path_.empty()) {
            return false;
        }
        EngineConfig cfg;
        cfg.max_dict_capacity = totalEntryLimit_;
        cfg.prune_target_size = totalEntryLimit_;
        lmdbEngine_ = std::make_unique<PersonalEngine>(cfg);
        if (!lmdbEngine_->OpenLmdb(path_, true)) {
            lmdbEngine_.reset();
            return false;
        }
        return true;
    }

    if (db_ != nullptr) {
        return true;
    }
    if (path_.empty() ||
        sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 0);
    return true;
}

bool LearningStore::migrateFromSqlite(const std::filesystem::path &sqlitePath) {
    if (!useLmdb_) {
        return false;
    }
    if ((lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) && !open()) {
        return false;
    }
    return lmdbEngine_->MigrateFromSqlite(sqlitePath);
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
    if (lmdbEngine_ != nullptr) {
        lmdbEngine_->CloseLmdb();
        lmdbEngine_.reset();
    }
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
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return false;
        }
        return lmdbEngine_->BackupLmdbTo(path);
    }

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
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return false;
        }
        return lmdbEngine_->Clear();
    }

    if (db_ == nullptr || !execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    if (!execute("DELETE FROM learning_entries;"
                 "UPDATE learning_state SET reset_revision = reset_revision + 1 "
                 "WHERE id = 1;")) {
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
    std::vector<std::string> clearKeys{normalized};
    const auto englishWord = detail::canonicalEnglishSelectionWord(phrase, pinyin);
    for (size_t length = 1; length < englishWord.size(); ++length) {
        clearKeys.push_back(englishWord.substr(0, length));
    }
    // Exact prefix keys use the existing (pinyin, phrase) index. No full-table
    // scan, migration, or frequency increment is needed to clear old aliases.
    std::string clearSuppressionSql =
        "UPDATE learning_entries SET suppressed = 0 WHERE phrase = ? AND pinyin IN (";
    for (size_t i = 0; i < clearKeys.size(); ++i) {
        clearSuppressionSql += i == 0 ? "?" : ",?";
    }
    clearSuppressionSql += ");";
    sqlite3_stmt *clearStatement = nullptr;
    if (sqlite3_prepare_v2(db_, clearSuppressionSql.c_str(), -1, &clearStatement,
                           nullptr) != SQLITE_OK) {
        return false;
    }
    bool clearBound = bindText(clearStatement, 1, phrase);
    for (size_t i = 0; i < clearKeys.size() && clearBound; ++i) {
        clearBound = bindText(clearStatement, static_cast<int>(i + 2), clearKeys[i]);
    }
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
    LearningEvent event;
    event.kind = LearningEvent::Kind::Selection;
    event.phrase = std::string(phrase);
    event.pinyin = std::string(pinyin);
    event.contextBefore = std::string(contextBefore);
    event.contextAfter = std::string(contextAfter);
    event.nowMs = nowMs;
    return recordBatch({event});
}

bool LearningStore::recordCommit(const CommitEvent &event) {
    const auto pinyinKey = event.fullPinyin.empty() ? event.text : event.fullPinyin;
    bool hasPositive = false;
    if (useLmdb_) {
        if (lmdbEngine_ != nullptr && lmdbEngine_->IsLmdbOpen()) {
            hasPositive = lmdbEngine_->HasPositiveFrequency(pinyinKey, event.text);
        }
    } else {
        const auto snap = snapshot();
        if (snap != nullptr) {
            hasPositive = snap->hasPositiveFrequency(event.text, pinyinKey);
        }
    }
    const auto decision = decideCommitLearning(event, hasPositive);
    if (decision != CommitLearningDecision::Record) {
        return true;
    }

    return recordSelection(event.text, pinyinKey, event.contextBefore,
                           event.contextAfter, event.timestampMs);
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
    LearningEvent event;
    event.kind = LearningEvent::Kind::NegativeFeedback;
    event.phrase = std::string(phrase);
    event.pinyin = std::string(pinyin);
    return recordBatch({event});
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
    LearningEvent event;
    event.kind = LearningEvent::Kind::Suppression;
    event.phrase = std::string(phrase);
    event.pinyin = std::string(pinyin);
    return recordBatch({event});
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
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return false;
        }
        if (events.empty()) {
            return true;
        }
        return lmdbEngine_->RecordLearningBatch(events);
    }

    if (db_ == nullptr) {
        return false;
    }
    if (events.empty()) {
        return true;
    }
    if (!execute("BEGIN IMMEDIATE;")) {
        return false;
    }
    const auto currentRevision = resetRevision();
    if (!currentRevision.has_value()) {
        execute("ROLLBACK;");
        return false;
    }
    for (const auto &event : events) {
        if (event.resetRevision.has_value() &&
            *event.resetRevision != *currentRevision) {
            continue;
        }
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
    if (faultInjection_ == FaultInjection::FailBeforeCommit) {
        execute("ROLLBACK;");
        return false;
    }
    // Keep housekeeping in the same transaction as the events. Otherwise a
    // pruning failure can be reported after COMMIT, and LearningWriter's safe
    // retry would apply already-committed selections a second time.
    if (faultInjection_ == FaultInjection::FailDuringPrune ||
        !pruneContextVariants() || !pruneTotalEntries()) {
        execute("ROLLBACK;");
        return false;
    }
    if (!execute("COMMIT;")) {
        execute("ROLLBACK;");
        return false;
    }
    return true;
}

std::optional<std::int64_t> LearningStore::resetRevision() const {
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return std::nullopt;
        }
        return lmdbEngine_->ResetRevision();
    }

    if (db_ == nullptr) {
        return std::nullopt;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT reset_revision FROM learning_state WHERE id = 1;",
                         -1, &statement, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    std::optional<std::int64_t> revision;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        revision = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return revision;
}

int LearningStore::schemaVersion() const {
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return 0;
        }
        return lmdbEngine_->SchemaVersion();
    }

    if (db_ == nullptr) {
        return 0;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version;", -1, &statement,
                           nullptr) != SQLITE_OK) {
        return 0;
    }
    int version = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        version = sqlite3_column_int(statement, 0);
    }
    sqlite3_finalize(statement);
    return version;
}

std::optional<LearningStoreState> LearningStore::readState() const {
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return std::nullopt;
        }
        std::vector<LearningEntry> entries;
        int64_t revision = 0;
        if (!lmdbEngine_->ReadLearningState(entries, revision)) {
            return std::nullopt;
        }
        return LearningStoreState{
            std::make_shared<const LearningSnapshot>(std::move(entries), totalEntryLimit_),
            revision};
    }

    if (!execute("BEGIN;")) {
        return std::nullopt;
    }
    const auto revision = resetRevision();
    auto loaded = revision.has_value() ? snapshot() : nullptr;
    if (loaded == nullptr || !execute("COMMIT;")) {
        execute("ROLLBACK;");
        return std::nullopt;
    }
    return LearningStoreState{std::move(loaded), *revision};
}

std::shared_ptr<const LearningSnapshot> LearningStore::snapshot(
    std::int64_t) const {
    if (useLmdb_) {
        if (lmdbEngine_ == nullptr || !lmdbEngine_->IsLmdbOpen()) {
            return std::make_shared<const LearningSnapshot>();
        }
        const auto state = readState();
        return state ? state->snapshot : nullptr;
    }

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
