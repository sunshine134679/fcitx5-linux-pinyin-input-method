#pragma once

#include "learning_boost.h"
#include "modernime/core/learning_snapshot.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modernime::core {

struct EnglishRecallEvidence {
    std::string lower;
    std::string text;
    LearningBoostEvidence learning;
};

// One policy for the entire snapshot. Shards supply evidence and conservative
// bounds; only this query applies dormancy, completeness and the final Top K.
class EnglishRecallQuery {
public:
    EnglishRecallQuery(std::string prefix, std::size_t limit, std::int64_t now);
    void observeLatest(const EnglishRecallEvidence &word);
    void consider(const EnglishRecallEvidence &word);
    std::vector<RecalledEnglishWord> results() const;

private:
    friend class EnglishRecallIndex;
    struct RankedWord {
        const EnglishRecallEvidence *word;
        double score;
        double boost;
    };
    static bool better(const RankedWord &a, const RankedWord &b);
    double score(double boost, std::int64_t last, std::size_t length, bool exact) const;
    std::string prefix_;
    std::size_t limit_;
    std::int64_t now_;
    std::int64_t latestMs_ = 0;
    double latestBoost_ = 0.0;
    std::vector<RankedWord> best_;
};

// Immutable, derived shard index. Bounds prune work, never the eligible word
// set: no alphabetical scan cap or per-shard ranking budget discards evidence.
class EnglishRecallIndex {
public:
    explicit EnglishRecallIndex(std::vector<EnglishRecallEvidence> words = {});
    void observeLatest(EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const;
    void collect(EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const;

private:
    struct Node {
        std::size_t begin = 0, end = 0, left = 0, right = 0;
        std::size_t shortest = 0, firstText = 0;
        std::int64_t oldestPositiveMs = 0;
        LearningBoostEvidence upper;
        bool hasUnstamped = false;
    };
    std::size_t build(std::size_t begin, std::size_t end);
    std::pair<std::size_t, std::size_t> range(std::string_view prefix) const;
    void visitLatest(std::size_t node, std::size_t begin, std::size_t end,
                     EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const;
    void visitBest(std::size_t node, std::size_t begin, std::size_t end,
                   EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const;
    double scoreUpper(const Node &node, const EnglishRecallQuery &query) const;
    bool canBeat(const Node &node, const EnglishRecallQuery &query) const;
    std::vector<EnglishRecallEvidence> words_;
    std::vector<Node> nodes_;
};

} // namespace modernime::core
