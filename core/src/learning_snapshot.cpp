#include "modernime/core/learning_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace modernime::core {

namespace {

std::int64_t saturatingIncrement(std::int64_t value) {
    if (value < 0 || value == std::numeric_limits<std::int64_t>::max()) {
        return value < 0 ? 1 : value;
    }
    return value + 1;
}

bool isUtf8Continuation(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

std::size_t utf8CharacterCount(std::string_view value) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!isUtf8Continuation(static_cast<unsigned char>(value[index]))) {
            ++count;
        }
    }
    return count;
}

std::string_view prefixCharacters(std::string_view value,
                                  std::size_t maxCharacters) {
    std::size_t end = 0;
    std::size_t count = 0;
    while (end < value.size() && count < maxCharacters) {
        ++end;
        while (end < value.size() &&
               isUtf8Continuation(static_cast<unsigned char>(value[end]))) {
            ++end;
        }
        ++count;
    }
    return value.substr(0, end);
}

std::string_view suffixCharacters(std::string_view value,
                                  std::size_t maxCharacters) {
    std::size_t begin = value.size();
    std::size_t count = 0;
    while (begin > 0 && count < maxCharacters) {
        --begin;
        while (begin > 0 &&
               isUtf8Continuation(static_cast<unsigned char>(value[begin]))) {
            --begin;
        }
        ++count;
    }
    return value.substr(begin);
}

std::size_t commonPrefixCharacters(std::string_view left,
                                   std::string_view right,
                                   std::size_t maximum) {
    const auto limit = std::min(
        {maximum, utf8CharacterCount(left), utf8CharacterCount(right)});
    for (std::size_t count = limit; count >= 1; --count) {
        if (prefixCharacters(left, count) == prefixCharacters(right, count)) {
            return count;
        }
    }
    return 0;
}

std::size_t commonSuffixCharacters(std::string_view left,
                                   std::string_view right,
                                   std::size_t maximum) {
    const auto limit = std::min(
        {maximum, utf8CharacterCount(left), utf8CharacterCount(right)});
    for (std::size_t count = limit; count >= 1; --count) {
        if (suffixCharacters(left, count) == suffixCharacters(right, count)) {
            return count;
        }
    }
    return 0;
}

std::string candidateKey(std::string_view phrase, std::string_view pinyin) {
    std::string key;
    key.reserve(phrase.size() + pinyin.size() + 1);
    key.append(phrase);
    key.push_back('\x1f');
    key.append(pinyin);
    return key;
}

} // namespace

std::string normalizePinyin(std::string_view pinyin) {
    std::string normalized;
    normalized.reserve(pinyin.size());
    for (const unsigned char character : pinyin) {
        if (character == '\'') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

LearningSnapshot::LearningSnapshot(std::vector<LearningEntry> entries,
                                   std::size_t totalEntryLimit)
    : entries_(std::move(entries)),
      totalEntryLimit_(totalEntryLimit == 0 ? kMaxLearningEntries
                                            : totalEntryLimit) {
    pruneContextVariants();
    pruneTotalEntries();
}

const LearningEntry *LearningSnapshot::entry(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    const auto normalized = normalizePinyin(pinyin);
    const LearningEntry *base = nullptr;
    for (const auto &candidate : entries_) {
        if (candidate.phrase != phrase || candidate.pinyin != normalized) {
            continue;
        }
        if (candidate.contextBefore == contextBefore &&
            candidate.contextAfter == contextAfter) {
            return &candidate;
        }
        if (candidate.contextBefore.empty() && candidate.contextAfter.empty()) {
            base = &candidate;
        }
    }
    return base;
}

bool LearningSnapshot::isSuppressed(std::string_view phrase,
                                    std::string_view pinyin) const {
    const auto normalized = normalizePinyin(pinyin);
    for (const auto &candidate : entries_) {
        if (candidate.phrase == phrase && candidate.pinyin == normalized &&
            candidate.suppressed) {
            return true;
        }
    }
    return false;
}

bool LearningSnapshot::hasPositiveFrequency(std::string_view phrase,
                                            std::string_view pinyin) const {
    if (isSuppressed(phrase, pinyin)) {
        return false;
    }
    const auto normalized = normalizePinyin(pinyin);
    for (const auto &item : entries_) {
        if (item.phrase == phrase && item.pinyin == normalized &&
            !item.suppressed && item.frequency > 0) {
            return true;
        }
    }
    return false;
}

double LearningSnapshot::boostAt(std::string_view phrase,
                                 std::string_view pinyin,
                                 std::int64_t nowMs) const {
    const auto normalized = normalizePinyin(pinyin);
    if (isSuppressed(phrase, normalized)) {
        return 0.0;
    }
    // Frequency is aggregated across every context variant: each selection
    // writes exactly one row, so the sum is the true "how often the user
    // picks this word" signal regardless of which sentence it appeared in.
    std::int64_t totalFrequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t worstFeedback = 0;
    for (const auto &item : entries_) {
        if (item.phrase != phrase || item.pinyin != normalized) {
            continue;
        }
        worstFeedback = std::max(worstFeedback, item.negativeFeedback);
        if (item.suppressed) {
            continue;
        }
        totalFrequency += std::max<std::int64_t>(0, item.frequency);
        lastSelectedMs = std::max(lastSelectedMs, item.lastSelectedMs);
    }
    if (totalFrequency <= 0) {
        return 0.0;
    }
    const double ageMs = std::max(
        0.0, static_cast<double>(nowMs) -
                 static_cast<double>(lastSelectedMs));
    constexpr double halfLifeMs = 30.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    const double recency = std::exp(-ageMs / halfLifeMs);
    // log1p growth reaches the cap around 25 accumulated selections, so
    // genuinely frequent picks climb steadily instead of saturating after a
    // couple of uses.
    const double frequency = std::min(
        3.0, 0.85 * std::log1p(static_cast<double>(totalFrequency)));
    const double recent = std::min(1.0, 0.90 * recency);
    // The strongest negative feedback of any variant applies globally.
    const double penalty = std::min(
        2.0, 0.75 * std::log1p(static_cast<double>(
                        std::max<std::int64_t>(0, worstFeedback))));
    return std::clamp(frequency + recent - penalty, -2.0, 4.0);
}

double LearningSnapshot::contextBoost(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    if (contextBefore.empty() && contextAfter.empty()) {
        return 0.0;
    }
    const auto normalized = normalizePinyin(pinyin);
    if (isSuppressed(phrase, normalized)) {
        return 0.0;
    }
    constexpr std::size_t contextWindow = 8;
    double bestBoost = 0.0;
    for (const auto &candidate : entries_) {
        if (candidate.phrase != phrase || candidate.pinyin != normalized) {
            continue;
        }
        if (candidate.contextBefore == contextBefore &&
            candidate.contextAfter == contextAfter &&
            !contextBefore.empty() && !contextAfter.empty()) {
            bestBoost = std::max(bestBoost, 0.8);
            continue;
        }
        if ((candidate.contextBefore == contextBefore &&
             !contextBefore.empty() && candidate.contextAfter.empty()) ||
            (candidate.contextAfter == contextAfter &&
             !contextAfter.empty() && candidate.contextBefore.empty())) {
            bestBoost = std::max(bestBoost, 0.4);
            continue;
        }
        const auto beforeMatch = commonSuffixCharacters(
            candidate.contextBefore, contextBefore, contextWindow);
        const auto afterMatch = commonPrefixCharacters(
            candidate.contextAfter, contextAfter, contextWindow);
        if (beforeMatch >= 4 && afterMatch >= 4) {
            bestBoost = std::max(bestBoost, 0.6);
        } else if (beforeMatch >= 2 && afterMatch >= 2) {
            bestBoost = std::max(bestBoost, 0.4);
        } else if (beforeMatch >= 4 || afterMatch >= 4) {
            bestBoost = std::max(bestBoost, 0.3);
        } else if (beforeMatch >= 2 || afterMatch >= 2) {
            bestBoost = std::max(bestBoost, 0.2);
        }
    }
    return bestBoost;
}

LearningEntry *LearningSnapshot::mutableEntry(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) {
    const auto normalized = normalizePinyin(pinyin);
    for (auto &candidate : entries_) {
        if (candidate.phrase == phrase && candidate.pinyin == normalized &&
            candidate.contextBefore == contextBefore &&
            candidate.contextAfter == contextAfter) {
            return &candidate;
        }
    }
    entries_.push_back({std::string(phrase), normalized,
                        std::string(contextBefore), std::string(contextAfter)});
    return &entries_.back();
}

void LearningSnapshot::recordSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    const auto normalized = normalizePinyin(pinyin);
    for (auto &item : entries_) {
        if (item.phrase == phrase && item.pinyin == normalized) {
            item.suppressed = false;
        }
    }
    auto *candidate = mutableEntry(phrase, pinyin, contextBefore, contextAfter);
    candidate->suppressed = false;
    candidate->frequency = saturatingIncrement(candidate->frequency);
    candidate->lastSelectedMs = nowMs;
    pruneContextVariants();
    pruneTotalEntries();
}

void LearningSnapshot::recordNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    auto *candidate = mutableEntry(phrase, pinyin, {}, {});
    candidate->negativeFeedback =
        saturatingIncrement(candidate->negativeFeedback);
    pruneTotalEntries();
}

void LearningSnapshot::recordSuppression(std::string_view phrase,
                                         std::string_view pinyin) {
    auto *candidate = mutableEntry(phrase, pinyin, {}, {});
    candidate->suppressed = true;
    candidate->negativeFeedback =
        saturatingIncrement(candidate->negativeFeedback);
    pruneTotalEntries();
}

void LearningSnapshot::pruneContextVariants() {
    constexpr std::size_t maximumVariants = 8;
    std::unordered_map<std::string, std::vector<std::size_t>> variants;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto &entry = entries_[index];
        if (entry.contextBefore.empty() && entry.contextAfter.empty()) {
            continue;
        }
        variants[candidateKey(entry.phrase, entry.pinyin)].push_back(index);
    }

    std::vector<bool> keep(entries_.size(), true);
    for (auto &[key, indexes] : variants) {
        (void)key;
        std::stable_sort(indexes.begin(), indexes.end(),
                         [this](std::size_t left, std::size_t right) {
                             const auto &a = entries_[left];
                             const auto &b = entries_[right];
                             if (a.suppressed != b.suppressed) {
                                 return a.suppressed > b.suppressed;
                             }
                             const auto aFrequency = std::max<std::int64_t>(
                                 0, a.frequency);
                             const auto bFrequency = std::max<std::int64_t>(
                                 0, b.frequency);
                             if (aFrequency != bFrequency) {
                                 return aFrequency > bFrequency;
                             }
                             if (a.lastSelectedMs != b.lastSelectedMs) {
                                 return a.lastSelectedMs > b.lastSelectedMs;
                             }
                             if (a.negativeFeedback != b.negativeFeedback) {
                                 return a.negativeFeedback > b.negativeFeedback;
                             }
                             return left < right;
                         });
        for (std::size_t position = maximumVariants; position < indexes.size();
             ++position) {
            keep[indexes[position]] = false;
        }
    }

    std::vector<LearningEntry> retained;
    retained.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (keep[index]) {
            retained.push_back(std::move(entries_[index]));
        }
    }
    entries_ = std::move(retained);
}

void LearningSnapshot::pruneTotalEntries() {
    if (entries_.size() <= totalEntryLimit_) {
        return;
    }
    std::vector<std::size_t> indexes(entries_.size());
    std::iota(indexes.begin(), indexes.end(), std::size_t{0});
    // Same retention priority as the SQLite-side prune: keep unsuppressed,
    // recently selected and frequently selected entries.
    const auto keepPriority = [this](std::size_t left, std::size_t right) {
        const auto &a = entries_[left];
        const auto &b = entries_[right];
        if (a.suppressed != b.suppressed) {
            return a.suppressed < b.suppressed;
        }
        if (a.lastSelectedMs != b.lastSelectedMs) {
            return a.lastSelectedMs > b.lastSelectedMs;
        }
        if (a.frequency != b.frequency) {
            return a.frequency > b.frequency;
        }
        return left < right;
    };
    std::partial_sort(indexes.begin(),
                      indexes.begin() + static_cast<std::ptrdiff_t>(
                                            totalEntryLimit_),
                      indexes.end(), keepPriority);
    std::vector<bool> keep(entries_.size(), false);
    for (std::size_t position = 0; position < totalEntryLimit_; ++position) {
        keep[indexes[position]] = true;
    }
    std::vector<LearningEntry> retained;
    retained.reserve(totalEntryLimit_);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (keep[index]) {
            retained.push_back(std::move(entries_[index]));
        }
    }
    entries_ = std::move(retained);
}

} // namespace modernime::core
