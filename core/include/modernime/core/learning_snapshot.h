#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
    LearningSnapshot();
    explicit LearningSnapshot(
        std::vector<LearningEntry> entries,
        std::size_t totalEntryLimit = kMaxLearningEntries);
    LearningSnapshot(const LearningSnapshot &other);
    LearningSnapshot &operator=(const LearningSnapshot &other);
    LearningSnapshot(LearningSnapshot &&other) noexcept = default;
    LearningSnapshot &operator=(LearningSnapshot &&other) noexcept = default;
    ~LearningSnapshot() = default;

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

    const std::vector<LearningEntry> &entries() const;

    void recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    void recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    void recordSuppression(std::string_view phrase,
                           std::string_view pinyin);

private:
    std::vector<const LearningEntry *> collectVariants(
        std::string_view phrase, std::string_view normalizedPinyin) const;
    void consolidate();
    static void pruneContextVariants(std::vector<LearningEntry> &entries);
    static void pruneTotalEntries(std::vector<LearningEntry> &entries, std::size_t limit);

    std::shared_ptr<const std::vector<LearningEntry>> baseEntries_;
    std::shared_ptr<const std::unordered_map<std::string, std::vector<std::size_t>>> baseIndex_;
    std::unordered_map<std::string, LearningEntry> delta_;
    std::unordered_map<std::string, std::vector<std::string>> deltaIndex_;
    std::size_t totalEntryLimit_ = kMaxLearningEntries;
    std::uint32_t selectionsSincePrune_ = 0;
    mutable std::vector<LearningEntry> materializedEntries_;
    mutable bool materializedDirty_ = true;
};

} // namespace modernime::core
