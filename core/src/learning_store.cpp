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
    "PRIMARY KEY (pinyin, phrase, context_before, context_after));";

bool bindText(sqlite3_stmt *statement, int index, std::string_view value) {
    return sqlite3_bind_text(statement, index, value.data(),
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
    if (!execute("PRAGMA journal_mode=WAL;") || !execute(schema)) {
        close();
        return false;
    }
    return true;
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

bool LearningStore::recordSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql =
        "INSERT INTO learning_entries "
        "(pinyin, phrase, context_before, context_after, frequency, "
        "last_selected_ms) VALUES (?, ?, ?, ?, 1, ?) "
        "ON CONFLICT(pinyin, phrase, context_before, context_after) DO "
        "UPDATE SET frequency = frequency + 1, last_selected_ms = excluded.last_selected_ms;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    const auto normalized = normalizePinyin(pinyin);
    const bool bound = bindText(statement, 1, normalized) &&
                       bindText(statement, 2, phrase) &&
                       bindText(statement, 3, contextBefore) &&
                       bindText(statement, 4, contextAfter) &&
                       sqlite3_bind_int64(statement, 5, nowMs) == SQLITE_OK;
    const bool success = bound && sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    return success;
}

bool LearningStore::recordNegativeFeedback(std::string_view phrase,
                                           std::string_view pinyin) {
    if (db_ == nullptr) {
        return false;
    }
    constexpr const char *sql =
        "INSERT INTO learning_entries "
        "(pinyin, phrase, negative_feedback) VALUES (?, ?, 1) "
        "ON CONFLICT(pinyin, phrase, context_before, context_after) DO "
        "UPDATE SET negative_feedback = negative_feedback + 1;";
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
        const bool success = event.kind == LearningEvent::Kind::Selection
                                 ? recordSelection(
                                       event.phrase, event.pinyin,
                                       event.contextBefore, event.contextAfter,
                                       event.nowMs)
                                 : recordNegativeFeedback(event.phrase,
                                                          event.pinyin);
        if (!success) {
            execute("ROLLBACK;");
            return false;
        }
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
        "last_selected_ms, negative_feedback FROM learning_entries;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto textAt = [statement](int index) {
                const auto *value = sqlite3_column_text(statement, index);
                return value != nullptr
                           ? std::string(reinterpret_cast<const char *>(value))
                           : std::string();
            };
            entries.push_back({textAt(0), textAt(1), textAt(2), textAt(3),
                               sqlite3_column_int64(statement, 4),
                               sqlite3_column_int64(statement, 5),
                               sqlite3_column_int64(statement, 6)});
        }
    }
    sqlite3_finalize(statement);
    return std::make_shared<const LearningSnapshot>(std::move(entries));
}

} // namespace modernime::core
