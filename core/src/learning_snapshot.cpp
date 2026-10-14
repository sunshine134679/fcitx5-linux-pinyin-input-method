#include "modernime/core/learning_snapshot.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <tuple>
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

} // namespace

std::string normalizePinyin(std::string_view pinyin) {
    if (pinyin.starts_with("!u!") || pinyin.starts_with("!U!")) {
        pinyin.remove_prefix(3);
    }
    std::string normalized;
    normalized.reserve(pinyin.size());
    for (const unsigned char character : pinyin) {
        if (character == '\'' || character == ',') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

namespace {

std::string candidateKey(std::string_view phrase, std::string_view pinyin) {
    const auto normPinyin = normalizePinyin(pinyin);
    std::string key;
    key.reserve(phrase.size() + normPinyin.size() + 1);
    key.append(phrase);
    key.push_back('\x1f');
    key.append(normPinyin);
    return key;
}

std::string variantKey(std::string_view phrase, std::string_view pinyin,
                       std::string_view contextBefore,
                       std::string_view contextAfter) {
    const auto normPinyin = normalizePinyin(pinyin);
    std::string key;
    key.reserve(phrase.size() + normPinyin.size() + contextBefore.size() +
                contextAfter.size() + 3);
    key.append(phrase);
    key.push_back('\x1f');
    key.append(normPinyin);
    key.push_back('\x1f');
    key.append(contextBefore);
    key.push_back('\x1f');
    key.append(contextAfter);
    return key;
}

} // namespace

bool isAsciiWord(std::string_view text) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    bool hasAlpha = false;
    for (char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            hasAlpha = true;
        } else if (c != '-' && c != '_') {
            return false;
        }
    }
    return hasAlpha;
}

std::string toLowerAscii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

struct LearningSnapshot::RecallIndex final {
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> canonicalIndex;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> abbrIndex;
    std::vector<std::tuple<std::string, std::string, std::string>> prefixIndex;
    std::vector<std::pair<std::string, std::string>> englishIndex;
};

struct LearningSnapshot::RecallIndexCache final {
    std::once_flag initialized;
    std::shared_ptr<const RecallIndex> index;
};

std::shared_ptr<const LearningSnapshot::RecallIndex>
LearningSnapshot::buildRecallIndex(const std::vector<LearningEntry> &entries) {
    auto index = std::make_shared<LearningSnapshot::RecallIndex>();
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> seenEnglish;
    seen.reserve(entries.size());
    for (const auto &entry : entries) {
        if (entry.suppressed || entry.frequency <= 0) {
            continue;
        }
        const bool asciiWord = isAsciiWord(entry.phrase);
        if (asciiWord) {
            const auto lower = toLowerAscii(entry.phrase);
            if (seenEnglish.insert(lower).second) {
                index->englishIndex.emplace_back(lower, entry.phrase);
            }
        }
        const std::string recallPinyin =
            asciiWord ? entry.pinyin : PinyinMatchPolicy::segmentPinyin(entry.pinyin);
        const auto key = candidateOrderKey(entry.phrase, recallPinyin);
        if (!seen.insert(key).second) {
            continue;
        }
        const auto canonical = PinyinMatchPolicy::canonical(recallPinyin);
        if (!canonical.empty()) {
            index->canonicalIndex[canonical].push_back({entry.phrase, recallPinyin});
            index->prefixIndex.emplace_back(canonical, entry.phrase, recallPinyin);
        }
        if (!asciiWord ||
            recallPinyin.find('\'') != std::string::npos ||
            recallPinyin.find(',') != std::string::npos) {
            const auto abbr = PinyinMatchPolicy::abbreviationKey(recallPinyin);
            if (abbr.size() >= 2) {
                index->abbrIndex[abbr].push_back({entry.phrase, recallPinyin});
            }
        }
    }
    std::sort(index->prefixIndex.begin(), index->prefixIndex.end(),
              [](const auto &a, const auto &b) {
                  return std::get<0>(a) < std::get<0>(b);
              });
    std::sort(index->englishIndex.begin(), index->englishIndex.end(),
              [](const auto &a, const auto &b) {
                  return a.first < b.first;
              });
    return index;
}

const LearningSnapshot::RecallIndex &LearningSnapshot::ensureRecallIndex() const {
    std::call_once(baseRecallIndex_->initialized, [this] {
        baseRecallIndex_->index = buildRecallIndex(*baseEntries_);
    });
    return *baseRecallIndex_->index;
}

LearningSnapshot::LearningSnapshot()
    : baseEntries_(std::make_shared<const std::vector<LearningEntry>>()),
      baseIndex_(std::make_shared<const std::unordered_map<std::string, std::vector<std::size_t>>>()),
      baseRecallIndex_(std::make_shared<RecallIndexCache>()) {
    ensureRecallIndex();
}

LearningSnapshot::LearningSnapshot(std::vector<LearningEntry> entries,
                                   std::size_t totalEntryLimit)
    : totalEntryLimit_(totalEntryLimit == 0 ? kMaxLearningEntries : totalEntryLimit) {
    pruneContextVariants(entries);
    pruneTotalEntries(entries, totalEntryLimit_);
    auto newIndex = std::make_shared<std::unordered_map<std::string, std::vector<std::size_t>>>();
    newIndex->reserve(entries.size() * 2);
    for (std::size_t idx = 0; idx < entries.size(); ++idx) {
        (*newIndex)[candidateKey(entries[idx].phrase, normalizePinyin(entries[idx].pinyin))].push_back(idx);
    }
    baseEntries_ = std::make_shared<const std::vector<LearningEntry>>(std::move(entries));
    baseIndex_ = std::move(newIndex);
    // Prepare snapshots loaded from storage before publication. Copies share
    // a cache whose initialization is synchronized, avoiding a lazy pointer
    // assignment racing a copy-on-write enqueue.
    baseRecallIndex_ = std::make_shared<RecallIndexCache>();
    ensureRecallIndex();
}

LearningSnapshot::LearningSnapshot(const LearningSnapshot &other)
    : baseEntries_(other.baseEntries_),
      baseIndex_(other.baseIndex_),
      baseRecallIndex_(other.baseRecallIndex_),
      delta_(other.delta_),
      deltaIndex_(other.deltaIndex_),
      totalEntryLimit_(other.totalEntryLimit_),
      selectionsSincePrune_(other.selectionsSincePrune_),
      materializedDirty_(true) {}

LearningSnapshot &LearningSnapshot::operator=(const LearningSnapshot &other) {
    if (this != &other) {
        baseEntries_ = other.baseEntries_;
        baseIndex_ = other.baseIndex_;
        baseRecallIndex_ = other.baseRecallIndex_;
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

std::size_t countUtf8Characters(std::string_view value) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while (offset < value.size()) {
        offset += utf8CharacterLengthAt(value, offset);
        ++count;
    }
    return count;
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
    std::int64_t genericFrequency = 0;
    std::int64_t lastSelectedMs = 0;
    std::int64_t worstFeedback = 0;
    bool suppressed = false;
    std::unordered_set<std::string> distinctContexts;
    for (const auto *item : variants) {
        worstFeedback = std::max(worstFeedback, item->negativeFeedback);
        if (item->suppressed) {
            suppressed = true;
            continue;
        }
        totalFrequency += std::max<std::int64_t>(0, item->frequency);
        lastSelectedMs = std::max(lastSelectedMs, item->lastSelectedMs);
        if (item->contextBefore.empty() && item->contextAfter.empty()) {
            genericFrequency += std::max<std::int64_t>(0, item->frequency);
        } else {
            distinctContexts.insert(item->contextBefore + "\x1f" + item->contextAfter);
        }
    }
    if (suppressed || totalFrequency <= 0) {
        return 0.0;
    }
    // 平滑与回退：若词条从未在无上下文或通用场景被选中，且仅局限在极少数（<3）特定上下文，
    // 其无条件频率按折扣衰减，避免单次或少数几次特定上下文训练覆盖全局常用排序。
    double effectiveFrequency = static_cast<double>(totalFrequency);
    if (genericFrequency == 0 && distinctContexts.size() < 3) {
        effectiveFrequency = std::max(0.0, 0.25 * static_cast<double>(totalFrequency));
    }

    const double ageMs = std::max(
        0.0, static_cast<double>(nowMs) -
                 static_cast<double>(lastSelectedMs));
    // 半衰期 T_1/2 与时间常数 tau 的物理换算：
    // exp(-delta_t / tau) = 2^(-delta_t / T_1/2)  ==> tau = T_1/2 / ln(2)
    // 默认半衰期为 30 天，严格保证当经过一个半衰期时衰减因子恰好为 0.5
    constexpr double halfLifeDays = 30.0;
    constexpr double halfLifeMs = halfLifeDays * 24.0 * 60.0 * 60.0 * 1000.0;
    constexpr double tauMs = halfLifeMs / 0.6931471805599453; // ln(2)
    const double recency = std::exp(-ageMs / tauMs);
    const double frequency = std::min(
        3.0, 0.85 * std::log1p(effectiveFrequency));
    const double recentScale =
        effectiveFrequency >= 3.0 ? 0.90 : (effectiveFrequency >= 2.0 ? 0.60 : (effectiveFrequency >= 1.0 ? 0.35 : 0.15));
    const double recent = std::min(1.0, recentScale * recency);
    const double penalty = std::min(
        2.0, 0.75 * std::log1p(static_cast<double>(
                        std::max<std::int64_t>(0, worstFeedback))));
    return std::clamp(frequency + recent - penalty, -2.0, 4.0);
}

std::int64_t LearningSnapshot::lastSelectedMs(
    std::string_view phrase, std::string_view pinyin) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    std::int64_t lastSelected = 0;
    for (const auto *item : variants) {
        if (!item->suppressed) {
            lastSelected = std::max(lastSelected, item->lastSelectedMs);
        }
    }
    return lastSelected;
}

double LearningSnapshot::contextBoost(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) const {
    if (contextBefore.empty() && contextAfter.empty()) {
        return 0.0;
    }
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectVariants(phrase, normalized);
    if (variants.empty()) {
        return 0.0;
    }
    std::int64_t worstFeedback = 0;
    for (const auto *item : variants) {
        if (item->suppressed) {
            return 0.0;
        }
        worstFeedback = std::max(worstFeedback, item->negativeFeedback);
    }
    constexpr std::size_t contextWindow = 8;
    double bestBoost = 0.0;
    for (const auto *item : variants) {
        if (item->frequency <= 0) {
            continue;
        }
        std::size_t beforeMatch = 0;
        bool isExactBefore = false;
        if (!contextBefore.empty() && !item->contextBefore.empty()) {
            if (item->contextBefore == contextBefore) {
                beforeMatch = countUtf8Characters(contextBefore);
                isExactBefore = true;
            } else {
                beforeMatch = commonSuffixCharacters(
                    item->contextBefore, contextBefore, contextWindow);
            }
        }

        std::size_t afterMatch = 0;
        bool isExactAfter = false;
        if (!contextAfter.empty() && !item->contextAfter.empty()) {
            if (item->contextAfter == contextAfter) {
                afterMatch = countUtf8Characters(contextAfter);
                isExactAfter = true;
            } else {
                afterMatch = commonPrefixCharacters(
                    item->contextAfter, contextAfter, contextWindow);
            }
        }

        if (beforeMatch == 0 && afterMatch == 0) {
            continue;
        }

        double matchScore = 0.0;
        if (beforeMatch >= 2) {
            matchScore = 0.80 + (isExactBefore ? 0.15 : 0.0) +
                         std::min(0.15, 0.05 * static_cast<double>(beforeMatch - 2));
        } else if (beforeMatch == 1) {
            matchScore = 0.65 + (isExactBefore ? 0.15 : 0.0);
        }
        if (afterMatch > 0) {
            matchScore += 0.10 * std::min<double>(3.0, static_cast<double>(afterMatch));
            if (isExactAfter) {
                matchScore += 0.05;
            }
        }

        double freqBonus = std::min(
            0.40, 0.20 * std::log1p(static_cast<double>(item->frequency)));
        if (nowMs > 0 && item->lastSelectedMs > 0) {
            const double ageMs = std::max(
                0.0, static_cast<double>(nowMs) -
                         static_cast<double>(item->lastSelectedMs));
            constexpr double halfLifeDays = 30.0;
            constexpr double halfLifeMs = halfLifeDays * 24.0 * 60.0 * 60.0 * 1000.0;
            constexpr double tauMs = halfLifeMs / 0.6931471805599453; // ln(2)
            const double recency = std::exp(-ageMs / tauMs);
            freqBonus *= (0.5 + 0.5 * recency);
        }
        const double itemBoost = std::min(1.5, matchScore + freqBonus);
        bestBoost = std::max(bestBoost, itemBoost);
    }
    if (worstFeedback > 0) {
        const double penalty = 0.5 * std::log1p(static_cast<double>(worstFeedback));
        bestBoost = std::max(0.0, bestBoost - penalty);
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
            newEntry.pinyin = pinyin;
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
        (*newIndex)[candidateKey(combined[idx].phrase, normalizePinyin(combined[idx].pinyin))].push_back(idx);
    }

    baseEntries_ = std::make_shared<const std::vector<LearningEntry>>(std::move(combined));
    baseIndex_ = std::move(newIndex);
    // Consolidation also runs on the input thread. Do not add a full recall
    // index rebuild to every 32nd selection or each suppression operation.
    baseRecallIndex_ = std::make_shared<RecallIndexCache>();
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

std::vector<RecalledCandidate> LearningSnapshot::recallCandidates(
    std::string_view userInput, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter) const {
    if (userInput.empty()) {
        return {};
    }
    const auto canonicalInput = PinyinMatchPolicy::canonical(userInput);
    const bool isAbbr = PinyinMatchPolicy::isAbbreviationInput(userInput);

    std::vector<RecalledCandidate> results;
    std::unordered_set<std::string> seen;

    auto addCandidate = [&](std::string_view phrase, std::string_view pinyin) {
        const auto key = candidateOrderKey(phrase, pinyin);
        if (!seen.insert(key).second) {
            return;
        }
        if (isSuppressed(phrase, pinyin) || !hasPositiveFrequency(phrase, pinyin)) {
            return;
        }
        const double lBoost = boostAt(phrase, pinyin, nowMs);
        const double cBoost = contextBoost(phrase, pinyin, contextBefore, contextAfter);
        results.push_back({std::string(phrase), std::string(pinyin), lBoost, cBoost});
    };

    const auto &baseRecall = ensureRecallIndex();
    if (!canonicalInput.empty()) {
        const auto it = baseRecall.canonicalIndex.find(canonicalInput);
        if (it != baseRecall.canonicalIndex.end()) {
            for (const auto &[phrase, pinyin] : it->second) {
                addCandidate(phrase, pinyin);
            }
        }
    }

    if (isAbbr) {
        const auto it = baseRecall.abbrIndex.find(std::string(userInput));
        if (it != baseRecall.abbrIndex.end()) {
            for (const auto &[phrase, pinyin] : it->second) {
                addCandidate(phrase, pinyin);
            }
        }
    }

    if (canonicalInput.size() >= 3) {
        auto it = std::lower_bound(
            baseRecall.prefixIndex.begin(),
            baseRecall.prefixIndex.end(),
            canonicalInput,
            [](const auto &tupleItem, const std::string &val) {
                return std::get<0>(tupleItem) < val;
            });
        std::vector<RecalledCandidate> prefixCandidates;
        std::size_t scanned = 0;
        constexpr std::size_t kMaxScan = 256;
        while (it != baseRecall.prefixIndex.end() &&
               std::get<0>(*it).starts_with(canonicalInput) &&
               scanned < kMaxScan) {
            const auto &phrase = std::get<1>(*it);
            const auto &pinyin = std::get<2>(*it);
            const auto key = candidateOrderKey(phrase, pinyin);
            if (!seen.contains(key) && !isSuppressed(phrase, pinyin) &&
                hasPositiveFrequency(phrase, pinyin)) {
                const double lBoost = boostAt(phrase, pinyin, nowMs);
                const double cBoost =
                    contextBoost(phrase, pinyin, contextBefore, contextAfter, nowMs);
                prefixCandidates.push_back({phrase, pinyin, lBoost, cBoost});
            }
            ++it;
            ++scanned;
        }
        constexpr std::size_t kMaxPrefixRecalled = 16;
        if (prefixCandidates.size() > kMaxPrefixRecalled) {
            std::partial_sort(
                prefixCandidates.begin(),
                prefixCandidates.begin() +
                    static_cast<std::ptrdiff_t>(kMaxPrefixRecalled),
                prefixCandidates.end(),
                [](const auto &a, const auto &b) {
                    const double scoreA = a.learningBoost + a.contextBoost;
                    const double scoreB = b.learningBoost + b.contextBoost;
                    if (std::abs(scoreA - scoreB) > 1e-4) {
                        return scoreA > scoreB;
                    }
                    if (a.phrase != b.phrase) {
                        return a.phrase < b.phrase;
                    }
                    return a.pinyin < b.pinyin;
                });
            prefixCandidates.resize(kMaxPrefixRecalled);
        }
        for (auto &cand : prefixCandidates) {
            const auto key = candidateOrderKey(cand.phrase, cand.pinyin);
            if (seen.insert(key).second) {
                results.push_back(std::move(cand));
            }
        }
    }

    for (const auto &[vKey, entry] : delta_) {
        if (entry.suppressed || entry.frequency <= 0) {
            continue;
        }
        const bool asciiWord = isAsciiWord(entry.phrase);
        const std::string recallPinyin =
            asciiWord ? entry.pinyin : PinyinMatchPolicy::segmentPinyin(entry.pinyin);
        const auto canonicalPinyin = PinyinMatchPolicy::canonical(recallPinyin);
        bool match = false;
        if (!canonicalInput.empty() && canonicalPinyin == canonicalInput) {
            match = true;
        } else if (isAbbr &&
                   (PinyinMatchPolicy::abbreviationKey(recallPinyin) == userInput ||
                    PinyinMatchPolicy::initialsMatch(userInput, recallPinyin))) {
            match = true;
        } else if (canonicalInput.size() >= 3 && canonicalPinyin.starts_with(canonicalInput)) {
            match = true;
        }
        if (match) {
            addCandidate(entry.phrase, recallPinyin);
        }
    }

    return results;
}

std::vector<RecalledEnglishWord> LearningSnapshot::recallEnglishWords(
    std::string_view prefix, std::size_t limit, std::int64_t nowMs) const {
    if (prefix.size() < 2) {
        return {};
    }
    const auto lowerPrefix = toLowerAscii(prefix);
    std::unordered_set<std::string> matchedPhrases;
    std::vector<std::string> candidates;

    const auto &baseRecall = ensureRecallIndex();
    auto it = std::lower_bound(
        baseRecall.englishIndex.begin(),
        baseRecall.englishIndex.end(),
        lowerPrefix,
        [](const auto &pair, const std::string &val) {
            return pair.first < val;
        });
    while (it != baseRecall.englishIndex.end() &&
           it->first.starts_with(lowerPrefix)) {
        if (matchedPhrases.insert(it->second).second) {
            candidates.push_back(it->second);
        }
        ++it;
    }

    for (const auto &[vKey, entry] : delta_) {
        if (entry.suppressed || entry.frequency <= 0) {
            continue;
        }
        if (isAsciiWord(entry.phrase)) {
            const auto lower = toLowerAscii(entry.phrase);
            if (lower.starts_with(lowerPrefix)) {
                if (matchedPhrases.insert(entry.phrase).second) {
                    candidates.push_back(entry.phrase);
                }
            }
        }
    }

    if (candidates.empty()) {
        return {};
    }

    std::vector<RecalledEnglishWord> results;
    results.reserve(candidates.size());
    for (const auto &phrase : candidates) {
        if (phrase.size() <= prefix.size()) {
            continue;
        }
        if (isSuppressed(phrase, phrase) || !hasPositiveFrequency(phrase, phrase)) {
            continue;
        }
        const double baseBoost = boostAt(phrase, phrase, nowMs);
        const auto lower = toLowerAscii(phrase);
        double completeness = 0.0;
        if (!lower.empty()) {
            completeness = static_cast<double>(lowerPrefix.size()) /
                           static_cast<double>(lower.size());
        }
        if (lower == lowerPrefix) {
            completeness += 1.0;
        }
        const double score = baseBoost + completeness;

        std::int64_t totalFreq = 0;
        std::int64_t lastMs = 0;
        for (const auto *item : collectVariants(phrase, normalizePinyin(phrase))) {
            totalFreq += std::max<std::int64_t>(0, item->frequency);
            lastMs = std::max(lastMs, item->lastSelectedMs);
        }
        results.push_back({phrase, totalFreq, lastMs, score});
    }

    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) {
                  if (std::abs(a.score - b.score) > 1e-6) {
                      return a.score > b.score;
                  }
                  if (a.frequency != b.frequency) {
                      return a.frequency > b.frequency;
                  }
                  if (a.lastSelectedMs != b.lastSelectedMs) {
                      return a.lastSelectedMs > b.lastSelectedMs;
                  }
                  return a.text < b.text;
              });

    if (results.size() > limit) {
        results.resize(limit);
    }
    return results;
}

} // namespace modernime::core
