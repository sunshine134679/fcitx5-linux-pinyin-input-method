#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::core {

// Upper bound on stored learning entries. Both the SQLite store and the
// in-memory snapshot evict beyond this limit: suppressed entries first, then
// the oldest and least frequently selected.
inline constexpr std::size_t kMaxLearningEntries = 200000;

struct LearningEntry final {
    std::string phrase;
    std::string pinyin;
    std::string contextBefore;
    std::string contextAfter;
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t negativeFeedback = 0;
    bool suppressed = false;
    bool operator==(const LearningEntry &) const = default;
};

// Administrative queries run on a captured immutable snapshot, outside the key path.
struct LearningQuery final {
    std::string search;
    std::optional<bool> suppressed;
    std::size_t offset = 0;
    std::size_t limit = 100;
};
struct LearningRecord final {
    std::string phrase;
    std::string pinyin;
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t negativeFeedback = 0;
    bool suppressed = false;
    std::size_t contextCount = 0;
};
struct LearningQueryPage final {
    std::vector<LearningRecord> records;
    std::size_t total = 0;
};

struct RecalledCandidate final {
    std::string phrase;
    std::string pinyin;
    double learningBoost = 0.0;
    double contextBoost = 0.0;
};

struct RecalledEnglishWord final {
    std::string text;
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    double score = 0.0;
    double baseBoost = 0.0;
};

std::string normalizePinyin(std::string_view pinyin);
bool learningEntriesEqual(const std::vector<LearningEntry> &left,
                          const std::vector<LearningEntry> &right);

class LearningSnapshotShard;

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
    std::int64_t lastSelectedMs(std::string_view phrase,
                                std::string_view pinyin) const;
    bool isSuppressed(std::string_view phrase,
                      std::string_view pinyin) const;
    double contextBoost(std::string_view phrase, std::string_view pinyin,
                        std::string_view contextBefore,
                        std::string_view contextAfter,
                        std::int64_t nowMs = 0) const;

    std::vector<RecalledCandidate> recallCandidates(
        std::string_view userInput, std::int64_t nowMs = 0,
        std::string_view contextBefore = {},
        std::string_view contextAfter = {}) const;

    // Indexed, bounded word-span lookup for request-local sentence decoding.
    // Inputs are full-pinyin readings, never raw shuangpin codes.
    std::vector<RecalledCandidate> recallSentenceWords(
        const std::vector<std::string> &readings, std::int64_t nowMs) const;

    // Includes a learned exact word so its evidence competes with completions.
    std::vector<RecalledEnglishWord> recallEnglishWords(
        std::string_view prefix, std::size_t limit = 5,
        std::int64_t nowMs = 0) const;

    const std::vector<LearningEntry> &entries() const;
    LearningQueryPage queryLearningEntries(const LearningQuery &query = {}) const;

    void recordSelection(std::string_view phrase, std::string_view pinyin,
                         std::string_view contextBefore,
                         std::string_view contextAfter, std::int64_t nowMs);
    void recordNegativeFeedback(std::string_view phrase,
                                std::string_view pinyin);
    void recordSuppression(std::string_view phrase,
                           std::string_view pinyin);

    // Exact normalized full-pinyin identity; every context variant is affected.
    void recordForget(std::string_view phrase, std::string_view pinyin);
    void recordUnsuppression(std::string_view phrase, std::string_view pinyin);

private:
    // Bound local index reconstruction at the unchanged 200k capacity.
    // 512 shards halve mutation/eviction work; recall still merges globally.
    static constexpr std::size_t kShardCount = 512;
    std::size_t shardFor(std::string_view phrase) const;
    LearningSnapshotShard &mutableShard(std::string_view phrase);
    void finishMutation(std::size_t previousCount, std::string_view phrase);
    std::array<std::shared_ptr<LearningSnapshotShard>, kShardCount> shards_{};
    std::size_t totalEntryLimit_ = kMaxLearningEntries;
    std::size_t entryCount_ = 0;
    struct EntriesCache;
    std::shared_ptr<EntriesCache> entriesCache_;
};

} // namespace modernime::core
