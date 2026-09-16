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

// 以下两个比较函数在原实现中每试一个长度就完整切一遍 UTF-8 前后缀
// （每次都产生新的 string_view 切片并逐字节比较），是 contextBoost 的
// 隐藏开销；改为从边界向内逐字符推进，不产生任何临时拷贝。
std::size_t utf8CharacterLengthAt(std::string_view value, std::size_t offset) {
    std::size_t length = 1;
    while (offset + length < value.size() &&
           isUtf8Continuation(
               static_cast<unsigned char>(value[offset + length]))) {
        ++length;
    }
    return length;
}

std::size_t commonPrefixCharacters(std::string_view left,
                                   std::string_view right,
                                   std::size_t maximum) {
    std::size_t count = 0;
    std::size_t leftOffset = 0;
    std::size_t rightOffset = 0;
    while (count < maximum && leftOffset < left.size() &&
           rightOffset < right.size()) {
        const auto leftLength = utf8CharacterLengthAt(left, leftOffset);
        const auto rightLength = utf8CharacterLengthAt(right, rightOffset);
        if (left.substr(leftOffset, leftLength) !=
            right.substr(rightOffset, rightLength)) {
            break;
        }
        leftOffset += leftLength;
        rightOffset += rightLength;
        ++count;
    }
    return count;
}

std::size_t commonSuffixCharacters(std::string_view left,
                                   std::string_view right,
                                   std::size_t maximum) {
    // 定位以 end 为结尾的 UTF-8 字符起点（跳过续字节）。
    const auto characterStart = [](std::string_view value,
                                   std::size_t end) {
        std::size_t start = end - 1;
        while (start > 0 &&
               isUtf8Continuation(
                   static_cast<unsigned char>(value[start]))) {
            --start;
        }
        return start;
    };
    std::size_t count = 0;
    std::size_t leftEnd = left.size();
    std::size_t rightEnd = right.size();
    while (count < maximum && leftEnd > 0 && rightEnd > 0) {
        const auto leftStart = characterStart(left, leftEnd);
        const auto rightStart = characterStart(right, rightEnd);
        if (left.substr(leftStart, leftEnd - leftStart) !=
            right.substr(rightStart, rightEnd - rightStart)) {
            break;
        }
        leftEnd = leftStart;
        rightEnd = rightStart;
        ++count;
    }
    return count;
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

void LearningSnapshot::ensureIndex() const {
    if (!indexDirty_) {
        return;
    }
    index_.clear();
    index_.reserve(entries_.size() * 2);
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        index_[candidateKey(entries_[index].phrase, entries_[index].pinyin)]
            .push_back(index);
    }
    indexDirty_ = false;
}

const std::vector<std::size_t> *LearningSnapshot::entryIndexes(
    std::string_view phrase, std::string_view normalizedPinyin) const {
    ensureIndex();
    const auto found = index_.find(candidateKey(phrase, normalizedPinyin));
    return found == index_.end() ? nullptr : &found->second;
}

const LearningEntry *LearningSnapshot::entry(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto *indexes = entryIndexes(phrase, normalized);
    if (indexes == nullptr) {
        return nullptr;
    }
    const LearningEntry *base = nullptr;
    for (const auto index : *indexes) {
        const auto &candidate = entries_[index];
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
    const auto *indexes = entryIndexes(phrase, normalized);
    if (indexes == nullptr) {
        return false;
    }
    for (const auto index : *indexes) {
        if (entries_[index].suppressed) {
            return true;
        }
    }
    return false;
}

bool LearningSnapshot::hasPositiveFrequency(std::string_view phrase,
                                            std::string_view pinyin) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto *indexes = entryIndexes(phrase, normalized);
    if (indexes == nullptr) {
        return false;
    }
    bool suppressed = false;
    for (const auto index : *indexes) {
        if (entries_[index].suppressed) {
            suppressed = true;
            break;
        }
    }
    if (suppressed) {
        return false;
    }
    for (const auto index : *indexes) {
        if (entries_[index].frequency > 0) {
            return true;
        }
    }
    return false;
}

double LearningSnapshot::boostAt(std::string_view phrase,
                                 std::string_view pinyin,
                                 std::int64_t nowMs) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto *indexes = entryIndexes(phrase, normalized);
    if (indexes == nullptr) {
        return 0.0;
    }
    // Frequency is aggregated across every context variant: each selection
    // writes exactly one row, so the sum is the true "how often the user
    // picks this word" signal regardless of which sentence it appeared in.
    std::int64_t totalFrequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t worstFeedback = 0;
    bool suppressed = false;
    for (const auto index : *indexes) {
        const auto &item = entries_[index];
        worstFeedback = std::max(worstFeedback, item.negativeFeedback);
        if (item.suppressed) {
            suppressed = true;
            continue;
        }
        totalFrequency += std::max<std::int64_t>(0, item.frequency);
        lastSelectedMs = std::max(lastSelectedMs, item.lastSelectedMs);
    }
    if (suppressed || totalFrequency <= 0) {
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
    const auto *indexes = entryIndexes(phrase, normalized);
    if (indexes == nullptr) {
        return 0.0;
    }
    bool suppressed = false;
    for (const auto index : *indexes) {
        if (entries_[index].suppressed) {
            suppressed = true;
            break;
        }
    }
    if (suppressed) {
        return 0.0;
    }
    constexpr std::size_t contextWindow = 8;
    double bestBoost = 0.0;
    for (const auto index : *indexes) {
        const auto &candidate = entries_[index];
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
    ensureIndex();
    auto &indexes = index_[candidateKey(phrase, normalized)];
    for (const auto index : indexes) {
        auto &candidate = entries_[index];
        if (candidate.contextBefore == contextBefore &&
            candidate.contextAfter == contextAfter) {
            return &candidate;
        }
    }
    entries_.push_back({std::string(phrase), normalized,
                        std::string(contextBefore), std::string(contextAfter)});
    // 新条目追加在末尾，现有下标不受影响，索引可增量维护。
    indexes.push_back(entries_.size() - 1);
    return &entries_.back();
}

void LearningSnapshot::recordSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    const auto normalized = normalizePinyin(pinyin);
    if (const auto *indexes = entryIndexes(phrase, normalized);
        indexes != nullptr) {
        for (const auto index : *indexes) {
            entries_[index].suppressed = false;
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
    // 重建后下标可能变化，索引整体失效。
    indexDirty_ = true;
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
    // 重建后下标可能变化，索引整体失效。
    indexDirty_ = true;
}

} // namespace modernime::core
