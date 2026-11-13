#include "english_recall_index.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace modernime::core {

EnglishRecallQuery::EnglishRecallQuery(std::string prefix, std::size_t limit, std::int64_t now)
    : prefix_(std::move(prefix)), limit_(limit), now_(now) {}

void EnglishRecallQuery::observeLatest(const EnglishRecallEvidence &word) {
    const auto last = word.learning.lastSelectedMs;
    if (last < latestMs_) return;
    const double boost = std::max(0.0, learningBoostAt(word.learning, now_));
    if (last > latestMs_) { latestMs_ = last; latestBoost_ = boost; }
    else latestBoost_ = std::max(latestBoost_, boost);
}

double EnglishRecallQuery::score(double boost, std::int64_t last, std::size_t length, bool exact) const {
    if (latestMs_ > 0 && last > 0 && last < latestMs_) {
        constexpr double tau = (12.0 * 3600.0 * 1000.0) / 0.6931471805599453;
        double dormant = std::exp(-(static_cast<double>(latestMs_) - static_cast<double>(last)) / tau);
        if (latestBoost_ >= 1.8)
            dormant = std::min(dormant, 1.0 - 0.30 * std::min(1.0, (latestBoost_ - 1.8) / (3.9 - 1.8)));
        boost *= dormant;
    }
    double completeness = static_cast<double>(prefix_.size()) / static_cast<double>(length);
    if (exact) completeness += 1.0;
    return boost + completeness;
}

bool EnglishRecallQuery::better(const RankedWord &a, const RankedWord &b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.word->learning.frequency != b.word->learning.frequency)
        return a.word->learning.frequency > b.word->learning.frequency;
    if (a.word->learning.lastSelectedMs != b.word->learning.lastSelectedMs)
        return a.word->learning.lastSelectedMs > b.word->learning.lastSelectedMs;
    return a.word->text < b.word->text;
}

void EnglishRecallQuery::consider(const EnglishRecallEvidence &word) {
    if (limit_ == 0) return;
    const double boost = learningBoostAt(word.learning, now_);
    RankedWord item{&word, score(boost, word.learning.lastSelectedMs, word.lower.size(), word.lower == prefix_), boost};
    const auto at = std::lower_bound(best_.begin(), best_.end(), item, better);
    if (best_.size() == limit_ && at == best_.end()) return;
    best_.insert(at, item);
    if (best_.size() > limit_) best_.pop_back();
}

std::vector<RecalledEnglishWord> EnglishRecallQuery::results() const {
    std::vector<RecalledEnglishWord> result;
    result.reserve(best_.size());
    for (const auto &item : best_)
        result.push_back({item.word->text, item.word->learning.frequency,
                          item.word->learning.lastSelectedMs, item.score, item.boost});
    return result;
}

EnglishRecallIndex::EnglishRecallIndex(std::vector<EnglishRecallEvidence> words) : words_(std::move(words)) {
    std::sort(words_.begin(), words_.end(), [](const auto &a, const auto &b) {
        if (a.lower != b.lower) return a.lower < b.lower;
        return a.text < b.text;
    });
    if (!words_.empty()) { nodes_.reserve(2 * words_.size() - 1); build(0, words_.size()); }
}

std::size_t EnglishRecallIndex::build(std::size_t begin, std::size_t end) {
    const auto position = nodes_.size();
    nodes_.push_back({});
    Node node;
    node.begin = begin; node.end = end;
    if (end == begin + 1) {
        node.shortest = words_[begin].lower.size(); node.firstText = begin;
        node.upper = words_[begin].learning;
        node.hasUnstamped = node.upper.lastSelectedMs <= 0;
        node.oldestPositiveMs = std::max<std::int64_t>(0, node.upper.lastSelectedMs);
    } else {
        const auto middle = begin + (end - begin) / 2;
        node.left = build(begin, middle); node.right = build(middle, end);
        const auto &a = nodes_[node.left]; const auto &b = nodes_[node.right];
        node.shortest = std::min(a.shortest, b.shortest);
        node.firstText = words_[a.firstText].text < words_[b.firstText].text ? a.firstText : b.firstText;
        node.upper = {std::max(a.upper.frequency, b.upper.frequency),
                      std::max(a.upper.lastSelectedMs, b.upper.lastSelectedMs),
                      std::max(a.upper.frequencyPart, b.upper.frequencyPart),
                      std::max(a.upper.recentScale, b.upper.recentScale),
                      std::min(a.upper.penalty, b.upper.penalty), false};
        node.hasUnstamped = a.hasUnstamped || b.hasUnstamped;
        node.oldestPositiveMs = a.oldestPositiveMs == 0 ? b.oldestPositiveMs :
            (b.oldestPositiveMs == 0 ? a.oldestPositiveMs : std::min(a.oldestPositiveMs, b.oldestPositiveMs));
    }
    nodes_[position] = node;
    return position;
}

std::pair<std::size_t, std::size_t> EnglishRecallIndex::range(std::string_view prefix) const {
    const auto begin = std::lower_bound(words_.begin(), words_.end(), prefix,
        [](const auto &word, std::string_view key) { return word.lower < key; });
    const auto end = std::partition_point(begin, words_.end(),
        [&](const auto &word) { return word.lower.starts_with(prefix); });
    return {static_cast<std::size_t>(begin - words_.begin()), static_cast<std::size_t>(end - words_.begin())};
}

void EnglishRecallIndex::observeLatest(EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const {
    const auto [begin, end] = range(query.prefix_);
    if (begin != end) visitLatest(0, begin, end, query, excluded);
}

void EnglishRecallIndex::visitLatest(std::size_t position, std::size_t begin, std::size_t end,
    EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const {
    const auto &node = nodes_[position];
    if (node.end <= begin || node.begin >= end || node.upper.lastSelectedMs < query.latestMs_) return;
    if (node.upper.lastSelectedMs == query.latestMs_ &&
        learningBoostAt(node.upper, query.now_) <= query.latestBoost_) return;
    if (node.end == node.begin + 1) {
        const auto &word = words_[node.begin];
        if (excluded.empty() || !excluded.contains(word.text)) query.observeLatest(word);
        return;
    }
    const bool leftFirst = nodes_[node.left].upper.lastSelectedMs >= nodes_[node.right].upper.lastSelectedMs;
    visitLatest(leftFirst ? node.left : node.right, begin, end, query, excluded);
    visitLatest(leftFirst ? node.right : node.left, begin, end, query, excluded);
}

double EnglishRecallIndex::scoreUpper(const Node &node, const EnglishRecallQuery &query) const {
    // All operations are monotone in these independent bounds. Keep the same
    // arithmetic order as the leaf score so equal-score ties can also prune.
    const double boost = learningBoostAt(node.upper, query.now_);
    // Multiplying a negative boost reverses the dormancy bound: use the
    // smallest possible factor, while a positive boost needs the largest.
    return query.score(boost,
                       boost < 0 ? node.oldestPositiveMs : (node.hasUnstamped ? 0 : node.upper.lastSelectedMs),
                       node.shortest, node.shortest <= query.prefix_.size());
}

bool EnglishRecallIndex::canBeat(const Node &node, const EnglishRecallQuery &query) const {
    if (query.best_.size() < query.limit_) return true;
    const auto &worst = query.best_.back();
    const double upper = scoreUpper(node, query);
    if (upper != worst.score) return upper > worst.score;
    if (node.upper.frequency != worst.word->learning.frequency)
        return node.upper.frequency > worst.word->learning.frequency;
    if (node.upper.lastSelectedMs != worst.word->learning.lastSelectedMs)
        return node.upper.lastSelectedMs > worst.word->learning.lastSelectedMs;
    return words_[node.firstText].text < worst.word->text;
}

void EnglishRecallIndex::collect(EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const {
    const auto [begin, end] = range(query.prefix_);
    if (begin != end && query.limit_ != 0) visitBest(0, begin, end, query, excluded);
}

void EnglishRecallIndex::visitBest(std::size_t position, std::size_t begin, std::size_t end,
    EnglishRecallQuery &query, const std::unordered_set<std::string> &excluded) const {
    const auto &node = nodes_[position];
    if (node.end <= begin || node.begin >= end || !canBeat(node, query)) return;
    if (node.end == node.begin + 1) {
        const auto &word = words_[node.begin];
        if (excluded.empty() || !excluded.contains(word.text)) query.consider(word);
        return;
    }
    const auto &a = nodes_[node.left]; const auto &b = nodes_[node.right];
    const double leftScore = scoreUpper(a, query), rightScore = scoreUpper(b, query);
    const bool leftFirst = leftScore != rightScore ? leftScore > rightScore :
        words_[a.firstText].text < words_[b.firstText].text;
    visitBest(leftFirst ? node.left : node.right, begin, end, query, excluded);
    visitBest(leftFirst ? node.right : node.left, begin, end, query, excluded);
}

} // namespace modernime::core
