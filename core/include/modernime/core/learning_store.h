#pragma once

#include "modernime/core/learning_snapshot.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace modernime::core {

struct LearningEvent final {
    enum class Kind { Selection, NegativeFeedback, Suppression };

    Kind kind = Kind::Selection;
    std::string phrase;
    std::string pinyin;
    std::string contextBefore;
    std::string contextAfter;
    std::int64_t nowMs = 0;
};

class LearningStore final {
public:
    explicit LearningStore(std::filesystem::path path);
    ~LearningStore();

    LearningStore(const LearningStore &) = delete;
    LearningStore &operator=(const LearningStore &) = delete;

    bool open();
    void close();
    bool recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    bool recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    bool recordSuppression(std::string_view phrase,
                           std::string_view pinyin);
    bool recordBatch(const std::vector<LearningEvent> &events);
    bool backupTo(const std::filesystem::path &path) const;
    bool clear();
    std::shared_ptr<const LearningSnapshot> snapshot(std::int64_t nowMs = 0) const;

private:
    bool execute(const char *sql) const;
    bool ensureSuppressionColumn() const;
    bool pruneContextVariants() const;

    std::filesystem::path path_;
    sqlite3 *db_ = nullptr;
};

} // namespace modernime::core
