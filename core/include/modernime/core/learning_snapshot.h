#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::core {

// Upper bound on stored learning entries. Both the SQLite store and the
// in-memory snapshot evict beyond this limit: suppressed entries first, then
// the oldest and least frequently selected.
inline constexpr std::size_t kMaxLearningEntries = 20000;

struct LearningEntry final {
    std::string phrase;
    std::string pinyin;
    std::string contextBefore;
    std::string contextAfter;
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t negativeFeedback = 0;
    bool suppressed = false;
};

std::string normalizePinyin(std::string_view pinyin);

class LearningSnapshot final {
public:
    LearningSnapshot() = default;
    explicit LearningSnapshot(
        std::vector<LearningEntry> entries,
        std::size_t totalEntryLimit = kMaxLearningEntries);

    const LearningEntry *entry(std::string_view phrase,
                               std::string_view pinyin,
                               std::string_view contextBefore,
                               std::string_view contextAfter) const;
    // Frequency aggregated across every context variant of (phrase, pinyin).
    bool hasPositiveFrequency(std::string_view phrase,
                              std::string_view pinyin) const;
    double boostAt(std::string_view phrase, std::string_view pinyin,
                   std::int64_t nowMs) const;
    bool isSuppressed(std::string_view phrase,
                      std::string_view pinyin) const;
    double contextBoost(std::string_view phrase, std::string_view pinyin,
                        std::string_view contextBefore,
                        std::string_view contextAfter) const;

    const std::vector<LearningEntry> &entries() const { return entries_; }

    void recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    void recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    void recordSuppression(std::string_view phrase,
                           std::string_view pinyin);

private:
    // 查询索引：key = phrase\x1fpinyin -> entries_ 下标。entries_ 发生
    // 增删或重建后置脏，下一次查询时重建一次；按键路径上的高频查询
    // （boostAt/contextBoost/isSuppressed/hasPositiveFrequency）全部走
    // 索引，避免对最多两万条学习记录做每键多次的全表线性扫描。
    void ensureIndex() const;
    const std::vector<std::size_t> *entryIndexes(
        std::string_view phrase,
        std::string_view normalizedPinyin) const;
    void pruneContextVariants();
    void pruneTotalEntries();
    LearningEntry *mutableEntry(std::string_view phrase,
                                std::string_view pinyin,
                                std::string_view contextBefore,
                                std::string_view contextAfter);

    std::vector<LearningEntry> entries_;
    std::size_t totalEntryLimit_ = kMaxLearningEntries;
    mutable std::unordered_map<std::string, std::vector<std::size_t>> index_;
    mutable bool indexDirty_ = true;
};

} // namespace modernime::core
