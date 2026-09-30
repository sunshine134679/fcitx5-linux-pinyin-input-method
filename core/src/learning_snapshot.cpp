#include "modernime/core/learning_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
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

std::string variantKey(std::string_view phrase, std::string_view pinyin,
                       std::string_view contextBefore,
                       std::string_view contextAfter) {
    std::string key;
    key.reserve(phrase.size() + pinyin.size() + contextBefore.size() +
                contextAfter.size() + 3);
    key.append(phrase);
    key.push_back('\x1f');
    key.append(pinyin);
    key.push_back('\x1f');
    key.append(contextBefore);
    key.push_back('\x1f');
    key.append(contextAfter);
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

LearningSnapshot::LearningSnapshot()
    : baseEntries_(std::make_shared<const std::vector<LearningEntry>>()),
      baseIndex_(std::make_shared<const std::unordered_map<std::string, std::vector<std::size_t>>>()) {}

LearningSnapshot::LearningSnapshot(std::vector<LearningEntry> entries,
                                   std::size_t totalEntryLimit)
    : totalEntryLimit_(totalEntryLimit == 0 ? kMaxLearningEntries : totalEntryLimit) {
    pruneContextVariants(entries);
    pruneTotalEntries(entries, totalEntryLimit_);
    auto newIndex = std::make_shared<std::unordered_map<std::string, std::vector<std::size_t>>>();
    newIndex->reserve(entries.size() * 2);
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        (*newIndex)[candidateKey(entries[idx].phrase, entries[idx].pinyin)].push_back(idx);
    }
    baseEntries_ = std::make_shared<const std::vector<LearningEntry>>(std::move(entries));
    baseIndex_ = std::move(newIndex);
}

LearningSnapshot::LearningSnapshot(const LearningSnapshot &other)
    : baseEntries_(other.baseEntries_),
      baseIndex_(other.baseIndex_),
      delta_(other.delta_),
      deltaIndex_(other.deltaIndex_),
      totalEntryLimit_(other.totalEntryLimit_),
      selectionsSincePrune_(other.selectionsSincePrune_),
      materializedDirty_(true) {}

LearningSnapshot &LearningSnapshot::operator=(const LearningSnapshot &other) {
    if (this != &other) {
        baseEntries_ = other.baseEntries_;
        baseIndex_ = other.baseIndex_;
        delta_ = other.delta_;
        deltaIndex_ = other.deltaIndex_;
        totalEntryLimit_ = other.totalEntryLimit_;
        selectionsSincePrune_ = other.selectionsSincePrune_;
        materializedDirty_ = true;
    }
    return *this;
}

std::vector<const LearningEntry *> LearningSnapshot::collectVariants(
    std::string_view phrase, std::string_view normalizedPinyin) const {
    std::vector<const LearningEntry *> result;
    const auto cKey = candidateKey(phrase, normalizedPinyin);
    std::unordered_set<std::string> seenVariants;

    if (baseIndex_ && baseEntries_) {
        const auto found = baseIndex_->find(cKey);
        if (found != baseIndex_->end()) {
            for (const auto index : found->second) {
                const auto &baseItem = (*baseEntries_)[index];
                const auto vKey = variantKey(baseItem.phrase, baseItem.pinyin,
                                             baseItem.contextBefore,
                                             baseItem.contextAfter);
                seenVariants.insert(vKey);
                const auto deltaFound = delta_.find(vKey);
                if (deltaFound != delta_.end()) {
                    result.push_back(&deltaFound->second);
                } else {
                    result.push_back(&baseItem);
                }
            }
        }
    }

    const auto deltaIndexFound = deltaIndex_.find(cKey);
    if (deltaIndexFound != deltaIndex_.end()) {
        for (const auto &vKey : deltaIndexFound->second) {
            if (!seenVariants.contains(vKey)) {
                const auto deltaFound = delta_.find(vKey);
                if (deltaFound != delta_.end()) {
                    result.push_back(&deltaFound->second);
                }
            }
        }
    }
    return result;
}

const LearningEntry *LearningSnapshot::entry(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    const LearningEntry *base = nullptr;
    for (const auto *cand : variants) {
        if (cand->contextBefore == contextBefore &&
            cand->contextAfter == contextAfter) {
            return cand;
        }
        if (cand->contextBefore.empty() && cand->contextAfter.empty()) {
            base = cand;
        }
    }
    return base;
}

bool LearningSnapshot::isSuppressed(std::string_view phrase,
                                    std::string_view pinyin) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    for (const auto *item : variants) {
        if (item->suppressed) {
            return true;
        }
    }
    return false;
}

bool LearningSnapshot::hasPositiveFrequency(std::string_view phrase,
                                            std::string_view pinyin) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    for (const auto *item : variants) {
        if (item->suppressed) {
            return false;
        }
    }
    for (const auto *item : variants) {
        if (item->frequency > 0) {
            return true;
        }
    }
    return false;
}

double LearningSnapshot::boostAt(std::string_view phrase,
                                 std::string_view pinyin,
                                 std::int64_t nowMs) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    if (variants.empty()) {
        return 0.0;
    }
    std::int64_t totalFrequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t worstFeedback = 0;
    bool suppressed = false;
    for (const auto *item : variants) {
        worstFeedback = std::max(worstFeedback, item->negativeFeedback);
        if (item->suppressed) {
            suppressed = true;
            continue;
        }
        totalFrequency += std::max<std::int64_t>(0, item->frequency);
        lastSelectedMs = std::max(lastSelectedMs, item->lastSelectedMs);
    }
    if (suppressed || totalFrequency <= 0) {
        return 0.0;
    }
    const double ageMs = std::max(
        0.0, static_cast<double>(nowMs) -
                 static_cast<double>(lastSelectedMs));
    constexpr double halfLifeMs = 30.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    const double recency = std::exp(-ageMs / halfLifeMs);
    const double frequency = std::min(
        3.0, 0.85 * std::log1p(static_cast<double>(totalFrequency)));
    const double recentScale =
        totalFrequency >= 3 ? 0.90 : (totalFrequency >= 2 ? 0.60 : 0.35);
    const double recent = std::min(1.0, recentScale * recency);
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
    const auto variants = collectVariants(phrase, normalized);
    if (variants.empty()) {
        return 0.0;
    }
    for (const auto *item : variants) {
        if (item->suppressed) {
            return 0.0;
        }
    }
    constexpr std::size_t contextWindow = 8;
    double bestBoost = 0.0;
    for (const auto *item : variants) {
        if (item->contextBefore == contextBefore &&
            item->contextAfter == contextAfter &&
            !contextBefore.empty() && !contextAfter.empty()) {
            bestBoost = std::max(bestBoost, 0.9);
            continue;
        }
        if ((item->contextBefore == contextBefore &&
             !contextBefore.empty() && item->contextAfter.empty()) ||
            (item->contextAfter == contextAfter &&
             !contextAfter.empty() && item->contextBefore.empty())) {
            bestBoost = std::max(bestBoost, 0.7);
            continue;
        }
        const auto beforeMatch = commonSuffixCharacters(
            item->contextBefore, contextBefore, contextWindow);
        const auto afterMatch = commonPrefixCharacters(
            item->contextAfter, contextAfter, contextWindow);
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

void LearningSnapshot::recordSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    const auto normalized = normalizePinyin(pinyin);
    const auto cKey = candidateKey(phrase, normalized);

    // 1. Unsuppress any suppressed variants of (phrase, normalized)
    const auto variants = collectVariants(phrase, normalized);
    for (const auto *item : variants) {
        if (item->suppressed) {
            const auto vKey = variantKey(item->phrase, item->pinyin,
                                         item->contextBefore, item->contextAfter);
            auto &d = delta_[vKey];
            d = *item;
            d.suppressed = false;
            auto &dList = deltaIndex_[cKey];
            if (std::find(dList.begin(), dList.end(), vKey) == dList.end()) {
                dList.push_back(vKey);
            }
        }
    }

    // 2. Insert or update targeted variant
    const auto targetVKey = variantKey(phrase, normalized, contextBefore, contextAfter);
    auto it = delta_.find(targetVKey);
    if (it != delta_.end()) {
        it->second.suppressed = false;
        it->second.frequency = saturatingIncrement(it->second.frequency);
        it->second.lastSelectedMs = nowMs;
    } else {
        const LearningEntry *baseEntry = nullptr;
        if (baseIndex_ && baseEntries_) {
            const auto found = baseIndex_->find(cKey);
            if (found != baseIndex_->end()) {
                for (const auto idx : found->second) {
                    const auto &cand = (*baseEntries_)[idx];
                    if (cand.contextBefore == contextBefore && cand.contextAfter == contextAfter) {
                        baseEntry = &cand;
                        break;
                    }
                }
            }
        }
        LearningEntry newEntry;
        if (baseEntry != nullptr) {
            newEntry = *baseEntry;
        } else {
            newEntry.phrase = phrase;
            newEntry.pinyin = normalized;
            newEntry.contextBefore = contextBefore;
            newEntry.contextAfter = contextAfter;
        }
        newEntry.suppressed = false;
        newEntry.frequency = saturatingIncrement(newEntry.frequency);
        newEntry.lastSelectedMs = nowMs;
        delta_[targetVKey] = std::move(newEntry);
        auto &dList = deltaIndex_[cKey];
        if (std::find(dList.begin(), dList.end(), targetVKey) == dList.end()) {
            dList.push_back(targetVKey);
        }
    }

    materializedDirty_ = true;
    ++selectionsSincePrune_;
    if (delta_.size() >= 32 || selectionsSincePrune_ >= 32 ||
        (baseEntries_ && baseEntries_->size() + delta_.size() > totalEntryLimit_)) {
        consolidate();
    }
}

void LearningSnapshot::recordNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    const auto normalized = normalizePinyin(pinyin);
    const auto cKey = candidateKey(phrase, normalized);
    const auto targetVKey = variantKey(phrase, normalized, {}, {});
    auto it = delta_.find(targetVKey);
    if (it != delta_.end()) {
        it->second.negativeFeedback = saturatingIncrement(it->second.negativeFeedback);
    } else {
        const LearningEntry *baseEntry = nullptr;
        if (baseIndex_ && baseEntries_) {
            const auto found = baseIndex_->find(cKey);
            if (found != baseIndex_->end()) {
                for (const auto idx : found->second) {
                    const auto &cand = (*baseEntries_)[idx];
                    if (cand.contextBefore.empty() && cand.contextAfter.empty()) {
                        baseEntry = &cand;
                        break;
                    }
                }
            }
        }
        LearningEntry newEntry;
        if (baseEntry != nullptr) {
            newEntry = *baseEntry;
        } else {
            newEntry.phrase = phrase;
            newEntry.pinyin = normalized;
        }
        newEntry.negativeFeedback = saturatingIncrement(newEntry.negativeFeedback);
        delta_[targetVKey] = std::move(newEntry);
        auto &dList = deltaIndex_[cKey];
        if (std::find(dList.begin(), dList.end(), targetVKey) == dList.end()) {
            dList.push_back(targetVKey);
        }
    }
    materializedDirty_ = true;
    consolidate();
}

void LearningSnapshot::recordSuppression(std::string_view phrase,
                                         std::string_view pinyin) {
    const auto normalized = normalizePinyin(pinyin);
    const auto cKey = candidateKey(phrase, normalized);
    const auto targetVKey = variantKey(phrase, normalized, {}, {});
    auto it = delta_.find(targetVKey);
    if (it != delta_.end()) {
        it->second.suppressed = true;
        it->second.negativeFeedback = saturatingIncrement(it->second.negativeFeedback);
    } else {
        const LearningEntry *baseEntry = nullptr;
        if (baseIndex_ && baseEntries_) {
            const auto found = baseIndex_->find(cKey);
            if (found != baseIndex_->end()) {
                for (const auto idx : found->second) {
                    const auto &cand = (*baseEntries_)[idx];
                    if (cand.contextBefore.empty() && cand.contextAfter.empty()) {
                        baseEntry = &cand;
                        break;
                    }
                }
            }
        }
        LearningEntry newEntry;
        if (baseEntry != nullptr) {
            newEntry = *baseEntry;
        } else {
            newEntry.phrase = phrase;
            newEntry.pinyin = normalized;
        }
        newEntry.suppressed = true;
        newEntry.negativeFeedback = saturatingIncrement(newEntry.negativeFeedback);
        delta_[targetVKey] = std::move(newEntry);
        auto &dList = deltaIndex_[cKey];
        if (std::find(dList.begin(), dList.end(), targetVKey) == dList.end()) {
            dList.push_back(targetVKey);
        }
    }
    materializedDirty_ = true;
    consolidate();
}

void LearningSnapshot::consolidate() {
    std::vector<LearningEntry> combined;
    std::unordered_set<std::string> seenKeys;
    if (baseEntries_) {
        combined.reserve(baseEntries_->size() + delta_.size());
        for (const auto &b : *baseEntries_) {
            const auto vKey = variantKey(b.phrase, b.pinyin, b.contextBefore, b.contextAfter);
            seenKeys.insert(vKey);
            const auto it = delta_.find(vKey);
            if (it != delta_.end()) {
                combined.push_back(it->second);
            } else {
                combined.push_back(b);
            }
        }
    }
    for (const auto &[vKey, entry] : delta_) {
        if (!seenKeys.contains(vKey)) {
            combined.push_back(entry);
        }
    }

    pruneContextVariants(combined);
    pruneTotalEntries(combined, totalEntryLimit_);

    auto newIndex = std::make_shared<std::unordered_map<std::string, std::vector<std::size_t>>>();
    newIndex->reserve(combined.size() * 2);
    for (std::size_t idx = 0; idx < combined.size(); ++idx) {
        (*newIndex)[candidateKey(combined[idx].phrase, combined[idx].pinyin)].push_back(idx);
    }

    baseEntries_ = std::make_shared<const std::vector<LearningEntry>>(std::move(combined));
    baseIndex_ = std::move(newIndex);
    delta_.clear();
    deltaIndex_.clear();
    selectionsSincePrune_ = 0;
    materializedDirty_ = true;
}

const std::vector<LearningEntry> &LearningSnapshot::entries() const {
    if (delta_.empty() && baseEntries_) {
        return *baseEntries_;
    }
    if (materializedDirty_) {
        materializedEntries_.clear();
        std::unordered_set<std::string> seenKeys;
        if (baseEntries_) {
            materializedEntries_.reserve(baseEntries_->size() + delta_.size());
            for (const auto &b : *baseEntries_) {
                const auto vKey = variantKey(b.phrase, b.pinyin, b.contextBefore, b.contextAfter);
                seenKeys.insert(vKey);
                const auto it = delta_.find(vKey);
                if (it != delta_.end()) {
                    materializedEntries_.push_back(it->second);
                } else {
                    materializedEntries_.push_back(b);
                }
            }
        }
        for (const auto &[vKey, entry] : delta_) {
            if (!seenKeys.contains(vKey)) {
                materializedEntries_.push_back(entry);
            }
        }
        materializedDirty_ = false;
    }
    return materializedEntries_;
}

void LearningSnapshot::pruneContextVariants(std::vector<LearningEntry> &entries) {
    constexpr std::size_t maximumVariants = 8;
    std::unordered_map<std::string, std::vector<std::size_t>> variants;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        if (entry.contextBefore.empty() && entry.contextAfter.empty()) {
            continue;
        }
        variants[candidateKey(entry.phrase, entry.pinyin)].push_back(index);
    }

    std::vector<bool> keep(entries.size(), true);
    for (auto &[key, indexes] : variants) {
        (void)key;
        std::stable_sort(indexes.begin(), indexes.end(),
                         [&entries](std::size_t left, std::size_t right) {
                             const auto &a = entries[left];
                             const auto &b = entries[right];
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
    retained.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (keep[index]) {
            retained.push_back(std::move(entries[index]));
        }
    }
    entries = std::move(retained);
}

void LearningSnapshot::pruneTotalEntries(std::vector<LearningEntry> &entries,
                                        std::size_t limit) {
    if (entries.size() <= limit) {
        return;
    }
    std::vector<std::size_t> indexes(entries.size());
    std::iota(indexes.begin(), indexes.end(), std::size_t{0});
    const auto keepPriority = [&entries](std::size_t left, std::size_t right) {
        const auto &a = entries[left];
        const auto &b = entries[right];
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
                      indexes.begin() + static_cast<std::ptrdiff_t>(limit),
                      indexes.end(), keepPriority);
    std::vector<bool> keep(entries.size(), false);
    for (std::size_t position = 0; position < limit; ++position) {
        keep[indexes[position]] = true;
    }
    std::vector<LearningEntry> retained;
    retained.reserve(limit);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (keep[index]) {
            retained.push_back(std::move(entries[index]));
        }
    }
    entries = std::move(retained);
}

} // namespace modernime::core
