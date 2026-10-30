#include "modernime/core/learning_snapshot.h"
#include "modernime/core/candidate_model.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
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

bool isRecallableEnglishWord(std::string_view text) {
    if (!isAsciiWord(text)) {
        return false;
    }
    if (EnglishDictionary::isEnglishWord(text)) {
        return true;
    }
    // Prevent Chinese pinyin sequences (like "jianpan", "nihao") from being treated as English words.
    if (PinyinMatchPolicy::isValidPinyinSequence(text)) {
        return false;
    }
    return true;
}

std::string toLowerAscii(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

class LearningSnapshotShard final {
    friend class LearningSnapshot;
public:
    LearningSnapshotShard();
    explicit LearningSnapshotShard(
        std::vector<LearningEntry> entries,
        std::size_t totalEntryLimit = kMaxLearningEntries);
    LearningSnapshotShard(const LearningSnapshotShard &other);
    LearningSnapshotShard &operator=(const LearningSnapshotShard &other);
    LearningSnapshotShard(LearningSnapshotShard &&other) noexcept = default;
    LearningSnapshotShard &operator=(LearningSnapshotShard &&other) noexcept = default;
    ~LearningSnapshotShard() = default;

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

    std::vector<RecalledEnglishWord> recallEnglishWords(
        std::string_view prefix, std::size_t limit = 5,
        std::int64_t nowMs = 0) const;

    const std::vector<LearningEntry> &entries() const;
    std::size_t entryCount() const {
        return (baseEntries_ ? baseEntries_->size() : 0) + deltaNewCount_;
    }
    const LearningEntry *weakestEntry() const;

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
    std::vector<const LearningEntry *> collectExactVariants(
        std::string_view phrase, std::string_view normalizedPinyin) const;
    void consolidate();
    static void pruneContextVariants(std::vector<LearningEntry> &entries);
    static void pruneTotalEntries(std::vector<LearningEntry> &entries, std::size_t limit);
    struct RecallIndex;
    struct RecallIndexCache;
    static std::shared_ptr<const RecallIndex> buildRecallIndex(const std::vector<LearningEntry> &entries);
    const RecallIndex &ensureRecallIndex() const;
    std::shared_ptr<const std::vector<LearningEntry>> baseEntries_;
    std::shared_ptr<const std::unordered_map<std::string, std::vector<std::size_t>>> baseIndex_;
    std::shared_ptr<RecallIndexCache> baseRecallIndex_;
    std::unordered_map<std::string, LearningEntry> delta_;
    std::unordered_map<std::string, std::vector<std::string>> deltaIndex_;
    std::size_t totalEntryLimit_ = kMaxLearningEntries;
    std::size_t deltaNewCount_ = 0;
    std::uint32_t selectionsSincePrune_ = 0;
    struct MaterializedCache final {
        std::once_flag initialized;
        std::vector<LearningEntry> entries;
        std::size_t weakestIndex = 0;
        bool hasWeakest = false;
    };
    std::shared_ptr<MaterializedCache> materializedCache_;
};


struct LearningSnapshotShard::RecallIndex final {
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> canonicalIndex;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> abbrIndex;
    std::vector<std::tuple<std::string, std::string, std::string>> prefixIndex;
    std::vector<std::string> abbreviationKeys;
    std::vector<std::pair<std::string, std::string>> englishIndex;
    std::unordered_map<std::string, std::vector<std::string>> englishKeys;
};

struct LearningSnapshotShard::RecallIndexCache final {
    std::once_flag initialized;
    std::shared_ptr<const RecallIndex> index;
};

std::shared_ptr<const LearningSnapshotShard::RecallIndex>
LearningSnapshotShard::buildRecallIndex(const std::vector<LearningEntry> &entries) {
    auto index = std::make_shared<LearningSnapshotShard::RecallIndex>();
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> seenEnglish;
    seen.reserve(entries.size());
    for (const auto &entry : entries) {
        const bool englishWord = isRecallableEnglishWord(entry.phrase);
        if (englishWord) {
            const auto key = normalizePinyin(entry.pinyin);
            if (!key.empty() && toLowerAscii(entry.phrase).starts_with(key)) {
                auto &keys = index->englishKeys[entry.phrase];
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
            }
        }
        if (entry.suppressed || entry.frequency <= 0) {
            continue;
        }
        if (englishWord) {
            const auto lower = toLowerAscii(entry.phrase);
            if (seenEnglish.insert(lower).second) {
                index->englishIndex.emplace_back(lower, entry.phrase);
            }
        }
        const std::string recallPinyin =
            englishWord ? entry.pinyin : PinyinMatchPolicy::segmentPinyin(entry.pinyin);
        const auto key = candidateOrderKey(entry.phrase, recallPinyin);
        if (!seen.insert(key).second) {
            continue;
        }
        const auto canonical = PinyinMatchPolicy::canonical(recallPinyin);
        if (!canonical.empty()) {
            index->canonicalIndex[canonical].push_back({entry.phrase, recallPinyin});
            index->prefixIndex.emplace_back(canonical, entry.phrase, recallPinyin);
        }
        if (!englishWord ||
            recallPinyin.find('\'') != std::string::npos ||
            recallPinyin.find(',') != std::string::npos) {
            const auto abbr = PinyinMatchPolicy::abbreviationKey(recallPinyin);
            if (abbr.size() >= 2) {
                index->abbrIndex[abbr].push_back({entry.phrase, recallPinyin});
            }
        }
    }
    for (const auto &[key, candidates] : index->abbrIndex) index->abbreviationKeys.push_back(key);
    std::sort(index->abbreviationKeys.begin(), index->abbreviationKeys.end());
    std::sort(index->prefixIndex.begin(), index->prefixIndex.end(),
              [](const auto &a, const auto &b) {
                  return a < b;
              });
    std::sort(index->englishIndex.begin(), index->englishIndex.end(),
              [](const auto &a, const auto &b) {
                  return a.first < b.first;
              });
    return index;
}

const LearningSnapshotShard::RecallIndex &LearningSnapshotShard::ensureRecallIndex() const {
    std::call_once(baseRecallIndex_->initialized, [this] {
        baseRecallIndex_->index = buildRecallIndex(*baseEntries_);
    });
    return *baseRecallIndex_->index;
}

LearningSnapshotShard::LearningSnapshotShard()
    : baseEntries_(std::make_shared<const std::vector<LearningEntry>>()),
      baseIndex_(std::make_shared<const std::unordered_map<std::string, std::vector<std::size_t>>>()),
      baseRecallIndex_(std::make_shared<RecallIndexCache>()),
      deltaNewCount_(0),
      materializedCache_(std::make_shared<MaterializedCache>()) {
    ensureRecallIndex();
}

LearningSnapshotShard::LearningSnapshotShard(std::vector<LearningEntry> entries,
                                   std::size_t totalEntryLimit)
    : totalEntryLimit_(totalEntryLimit == 0 ? kMaxLearningEntries : totalEntryLimit),
      deltaNewCount_(0),
      materializedCache_(std::make_shared<MaterializedCache>()) {
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

LearningSnapshotShard::LearningSnapshotShard(const LearningSnapshotShard &other)
    : baseEntries_(other.baseEntries_),
      baseIndex_(other.baseIndex_),
      baseRecallIndex_(other.baseRecallIndex_),
      delta_(other.delta_),
      deltaIndex_(other.deltaIndex_),
      totalEntryLimit_(other.totalEntryLimit_),
      deltaNewCount_(other.deltaNewCount_),
      selectionsSincePrune_(other.selectionsSincePrune_),
      materializedCache_(other.materializedCache_) {}

LearningSnapshotShard &LearningSnapshotShard::operator=(const LearningSnapshotShard &other) {
    if (this != &other) {
        baseEntries_ = other.baseEntries_;
        baseIndex_ = other.baseIndex_;
        baseRecallIndex_ = other.baseRecallIndex_;
        delta_ = other.delta_;
        deltaIndex_ = other.deltaIndex_;
        totalEntryLimit_ = other.totalEntryLimit_;
        deltaNewCount_ = other.deltaNewCount_;
        selectionsSincePrune_ = other.selectionsSincePrune_;
        materializedCache_ = other.materializedCache_;
    }
    return *this;
}

std::vector<const LearningEntry *> LearningSnapshotShard::collectExactVariants(
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

std::vector<const LearningEntry *> LearningSnapshotShard::collectVariants(
    std::string_view phrase, std::string_view normalizedPinyin) const {
    if (!isRecallableEnglishWord(phrase) || normalizedPinyin != toLowerAscii(phrase)) {
        return collectExactVariants(phrase, normalizedPinyin);
    }
    std::unordered_set<std::string> keys{std::string(normalizedPinyin)};
    const auto &index = ensureRecallIndex();
    const auto found = index.englishKeys.find(std::string(phrase));
    if (found != index.englishKeys.end()) keys.insert(found->second.begin(), found->second.end());
    for (const auto &[key, item] : delta_) {
        const auto py = normalizePinyin(item.pinyin);
        if (item.phrase == phrase && !py.empty() && toLowerAscii(phrase).starts_with(py)) keys.insert(py);
    }
    std::vector<const LearningEntry *> result;
    for (const auto &key : keys) {
        auto variants = collectExactVariants(phrase, key);
        result.insert(result.end(), variants.begin(), variants.end());
    }
    return result;
}

const LearningEntry *LearningSnapshotShard::entry(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    const auto normalized = normalizePinyin(pinyin);
    const auto variants = collectExactVariants(phrase, normalized);
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

bool LearningSnapshotShard::isSuppressed(std::string_view phrase,
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

bool LearningSnapshotShard::hasPositiveFrequency(std::string_view phrase,
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

double LearningSnapshotShard::boostAt(std::string_view phrase,
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

std::int64_t LearningSnapshotShard::lastSelectedMs(
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

double LearningSnapshotShard::contextBoost(
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

void LearningSnapshotShard::recordSelection(
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
    bool isNewEntry = false;
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
            isNewEntry = true;
            ++deltaNewCount_;
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

    materializedCache_ = std::make_shared<MaterializedCache>();
    ++selectionsSincePrune_;
    if (isNewEntry || delta_.size() >= 32) {
        consolidate();
    }
}

void LearningSnapshotShard::recordNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    const auto normalized = normalizePinyin(pinyin);
    const auto cKey = candidateKey(phrase, normalized);
    const auto targetVKey = variantKey(phrase, normalized, {}, {});
    auto it = delta_.find(targetVKey);
    bool isNewEntry = false;
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
            isNewEntry = true;
            ++deltaNewCount_;
        }
        newEntry.negativeFeedback = saturatingIncrement(newEntry.negativeFeedback);
        delta_[targetVKey] = std::move(newEntry);
        auto &dList = deltaIndex_[cKey];
        if (std::find(dList.begin(), dList.end(), targetVKey) == dList.end()) {
            dList.push_back(targetVKey);
        }
    }
    materializedCache_ = std::make_shared<MaterializedCache>();
    if (isNewEntry || delta_.size() >= 32) {
        consolidate();
    }
}

void LearningSnapshotShard::recordSuppression(std::string_view phrase,
                                         std::string_view pinyin) {
    const auto normalized = normalizePinyin(pinyin);
    const auto cKey = candidateKey(phrase, normalized);
    const auto targetVKey = variantKey(phrase, normalized, {}, {});
    auto it = delta_.find(targetVKey);
    bool isNewEntry = false;
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
            isNewEntry = true;
            ++deltaNewCount_;
        }
        newEntry.suppressed = true;
        newEntry.negativeFeedback = saturatingIncrement(newEntry.negativeFeedback);
        delta_[targetVKey] = std::move(newEntry);
        auto &dList = deltaIndex_[cKey];
        if (std::find(dList.begin(), dList.end(), targetVKey) == dList.end()) {
            dList.push_back(targetVKey);
        }
    }
    materializedCache_ = std::make_shared<MaterializedCache>();
    if (isNewEntry || delta_.size() >= 32) {
        consolidate();
    }
}

void LearningSnapshotShard::consolidate() {
    if (delta_.empty()) {
        return;
    }
    std::vector<LearningEntry> combined;
    std::unordered_set<std::string_view> unapplied;
    unapplied.reserve(delta_.size());
    for (const auto &[k, _] : delta_) {
        unapplied.insert(k);
    }
    if (baseEntries_) {
        combined.reserve(baseEntries_->size() + delta_.size());
        for (const auto &b : *baseEntries_) {
            if (!unapplied.empty()) {
                const auto vKey = variantKey(b.phrase, b.pinyin, b.contextBefore, b.contextAfter);
                const auto it = delta_.find(vKey);
                if (it != delta_.end()) {
                    combined.push_back(it->second);
                    unapplied.erase(vKey);
                    continue;
                }
            }
            combined.push_back(b);
        }
    }
    for (const auto &vKey : unapplied) {
        combined.push_back(delta_.find(std::string(vKey))->second);
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
    baseRecallIndex_ = std::make_shared<RecallIndexCache>();
    delta_.clear();
    deltaIndex_.clear();
    deltaNewCount_ = 0;
    selectionsSincePrune_ = 0;
    materializedCache_ = std::make_shared<MaterializedCache>();
}

const std::vector<LearningEntry> &LearningSnapshotShard::entries() const {
    if (delta_.empty() && baseEntries_) {
        return *baseEntries_;
    }
    const auto cache = materializedCache_;
    std::call_once(cache->initialized, [this, cache] {
        cache->entries.clear();
        std::unordered_set<std::string_view> unapplied;
        unapplied.reserve(delta_.size());
        for (const auto &[k, _] : delta_) {
            unapplied.insert(k);
        }
        if (baseEntries_) {
            cache->entries.reserve(baseEntries_->size() + delta_.size());
            for (const auto &b : *baseEntries_) {
                if (!unapplied.empty()) {
                    const auto vKey = variantKey(b.phrase, b.pinyin, b.contextBefore, b.contextAfter);
                    const auto it = delta_.find(vKey);
                    if (it != delta_.end()) {
                        cache->entries.push_back(it->second);
                        unapplied.erase(vKey);
                        continue;
                    }
                }
                cache->entries.push_back(b);
            }
        }
        for (const auto &vKey : unapplied) {
            cache->entries.push_back(delta_.find(std::string(vKey))->second);
        }
        if (!cache->entries.empty()) {
            cache->weakestIndex = 0;
            for (std::size_t i = 1; i < cache->entries.size(); ++i) {
                const auto &a = cache->entries[i];
                const auto &b = cache->entries[cache->weakestIndex];
                if (a.suppressed != b.suppressed ? a.suppressed :
                    a.lastSelectedMs != b.lastSelectedMs ? a.lastSelectedMs < b.lastSelectedMs :
                    a.frequency != b.frequency ? a.frequency < b.frequency : true) {
                    cache->weakestIndex = i;
                }
            }
            cache->hasWeakest = true;
        }
    });
    return cache->entries;
}

const LearningEntry *LearningSnapshotShard::weakestEntry() const {
    const auto &all = entries();
    if (all.empty()) return nullptr;
    const auto cache = materializedCache_;
    if (cache && cache->hasWeakest) {
        return &cache->entries[cache->weakestIndex];
    }
    return &all[0];
}

void LearningSnapshotShard::pruneContextVariants(std::vector<LearningEntry> &entries) {
    constexpr std::size_t maximumVariants = 8;
    std::unordered_map<std::string, std::vector<std::size_t>> variants;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        if (entry.contextBefore.empty() && entry.contextAfter.empty()) {
            continue;
        }
        variants[candidateKey(entry.phrase, entry.pinyin)].push_back(index);
    }
    if (variants.empty()) {
        return;
    }
    bool hasOverflow = false;
    for (const auto &[key, indexes] : variants) {
        if (indexes.size() > maximumVariants) {
            hasOverflow = true;
            break;
        }
    }
    if (!hasOverflow) {
        return;
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

void LearningSnapshotShard::pruneTotalEntries(std::vector<LearningEntry> &entries,
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

std::vector<RecalledEnglishWord> LearningSnapshotShard::recallEnglishWords(
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
        if (isRecallableEnglishWord(entry.phrase)) {
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
        std::int64_t totalFreq = 0;
        std::int64_t lastMs = 0;
        for (const auto *item : collectVariants(phrase, normalizePinyin(phrase))) {
            totalFreq += std::max<std::int64_t>(0, item->frequency);
            lastMs = std::max(lastMs, item->lastSelectedMs);
        }
        results.push_back({phrase, totalFreq, lastMs, 0.0, baseBoost});
    }

    std::int64_t latestMs = 0;
    double latestBoost = 0.0;
    for (const auto &item : results) {
        if (item.lastSelectedMs > latestMs) {
            latestMs = item.lastSelectedMs;
            latestBoost = std::isfinite(item.baseBoost) ? std::max(0.0, item.baseBoost) : 0.0;
        } else if (item.lastSelectedMs == latestMs) {
            const double boost = std::isfinite(item.baseBoost) ? std::max(0.0, item.baseBoost) : 0.0;
            latestBoost = std::max(latestBoost, boost);
        }
    }
    constexpr double kDormantHalfLifeMs = 12.0 * 3600.0 * 1000.0;
    constexpr double kDormantTauMs = kDormantHalfLifeMs / 0.6931471805599453;
    for (auto &item : results) {
        double effectiveBoost = item.baseBoost;
        if (latestMs > 0 && item.lastSelectedMs > 0 && item.lastSelectedMs < latestMs) {
            const auto ageSinceLatest = latestMs - item.lastSelectedMs;
            double dormant = std::exp(-static_cast<double>(ageSinceLatest) / kDormantTauMs);
            if (latestBoost >= 1.8) {
                const double discount = 1.0 - 0.30 * std::min(1.0, (latestBoost - 1.8) / (3.9 - 1.8));
                dormant = std::min(dormant, discount);
            }
            effectiveBoost = item.baseBoost * dormant;
        }
        const auto lower = toLowerAscii(item.text);
        double completeness = 0.0;
        if (!lower.empty()) {
            completeness = static_cast<double>(lowerPrefix.size()) /
                           static_cast<double>(lower.size());
        }
        if (lower == lowerPrefix) {
            completeness += 1.0;
        }
        item.score = effectiveBoost + completeness;
    }

    std::sort(results.begin(), results.end(),
              [](const auto &a, const auto &b) {
                  const bool aFin = std::isfinite(a.score);
                  const bool bFin = std::isfinite(b.score);
                  if (aFin != bFin) {
                      return aFin;
                  }
                  if (aFin && a.score != b.score) {
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

// Immutable snapshots share 512 independently indexed shards. A publication
// copies pointers and the affected shard only; the 200k vocabulary is never
// copied or reindexed merely because 32 selections have accumulated.
struct LearningSnapshot::EntriesCache final {
    std::once_flag initialized;
    std::vector<LearningEntry> entries;
};

LearningSnapshot::LearningSnapshot() : entriesCache_(std::make_shared<EntriesCache>()) {}
LearningSnapshot::LearningSnapshot(std::vector<LearningEntry> entries,
                                   std::size_t totalEntryLimit)
    : totalEntryLimit_(totalEntryLimit == 0 ? kMaxLearningEntries : totalEntryLimit),
      entriesCache_(std::make_shared<EntriesCache>()) {
    // Reuse the established global eviction policy when loading an oversized
    // store, then partition it. Context variants of one phrase stay together.
    if (entries.size() > totalEntryLimit_) {
        LearningSnapshotShard bounded(std::move(entries), totalEntryLimit_);
        entries = bounded.entries();
    }
    std::array<std::vector<LearningEntry>, kShardCount> partitions;
    for (auto &item : entries) partitions[shardFor(item.phrase)].push_back(std::move(item));
    for (std::size_t i = 0; i < kShardCount; ++i) {
        if (!partitions[i].empty()) {
            shards_[i] = std::make_shared<LearningSnapshotShard>(std::move(partitions[i]), totalEntryLimit_);
            entryCount_ += shards_[i]->entries().size();
            shards_[i]->weakestEntry();
        }
    }
}
LearningSnapshot::LearningSnapshot(const LearningSnapshot &other)
    : shards_(other.shards_), totalEntryLimit_(other.totalEntryLimit_), entryCount_(other.entryCount_), entriesCache_(other.entriesCache_) {}
LearningSnapshot &LearningSnapshot::operator=(const LearningSnapshot &other) {
    if (this != &other) {
        shards_ = other.shards_;
        totalEntryLimit_ = other.totalEntryLimit_;
        entryCount_ = other.entryCount_;
        entriesCache_ = other.entriesCache_;
    }
    return *this;
}
std::size_t LearningSnapshot::shardFor(std::string_view phrase) const {
    return std::hash<std::string_view>{}(phrase) % kShardCount;
}
LearningSnapshotShard &LearningSnapshot::mutableShard(std::string_view phrase) {
    auto &shard = shards_[shardFor(phrase)];
    if (!shard) shard = std::make_shared<LearningSnapshotShard>();
    else if (!shard.unique()) shard = std::make_shared<LearningSnapshotShard>(*shard);
    return *shard;
}
void LearningSnapshot::finishMutation(std::size_t previousCount, std::string_view phrase) {
    const auto currentCount = shards_[shardFor(phrase)]->entryCount();
    entryCount_ = entryCount_ - previousCount + currentCount;
    entriesCache_ = std::make_shared<EntriesCache>();
    if (entryCount_ > totalEntryLimit_) {
        shards_[shardFor(phrase)]->weakestEntry();
        while (entryCount_ > totalEntryLimit_) {
            // Each immutable shard publishes its weakest entry in advance. Global
            // eviction compares only 512 cached values, then rebuilds one shard.
            // Ties evict the last item in the materialized order, as before.
            std::size_t victimShard = kShardCount;
            const LearningEntry *weakest = nullptr;
            for (std::size_t i = 0; i < kShardCount; ++i) {
                if (!shards_[i]) continue;
                const auto *candidate = shards_[i]->weakestEntry();
                if (!candidate) continue;
                if (!weakest || (candidate->suppressed != weakest->suppressed ? candidate->suppressed :
                    candidate->lastSelectedMs != weakest->lastSelectedMs ? candidate->lastSelectedMs < weakest->lastSelectedMs :
                    candidate->frequency != weakest->frequency ? candidate->frequency < weakest->frequency : true)) {
                    victimShard = i;
                    weakest = candidate;
                }
            }
            const auto &original = shards_[victimShard]->entries();
            const auto victim = static_cast<std::size_t>(weakest - original.data());
            auto retained = original;
            retained.erase(retained.begin() + static_cast<std::ptrdiff_t>(victim));
            shards_[victimShard] = std::make_shared<LearningSnapshotShard>(std::move(retained), totalEntryLimit_);
            shards_[victimShard]->weakestEntry();
            --entryCount_;
        }
    }
}
const LearningEntry *LearningSnapshot::entry(std::string_view phrase, std::string_view pinyin,
                                            std::string_view before, std::string_view after) const {
    const auto &s = shards_[shardFor(phrase)];
    return s ? s->entry(phrase, pinyin, before, after) : nullptr;
}
bool LearningSnapshot::hasPositiveFrequency(std::string_view phrase, std::string_view pinyin) const {
    const auto &s = shards_[shardFor(phrase)];
    return s && s->hasPositiveFrequency(phrase, pinyin);
}
bool LearningSnapshot::isSuppressed(std::string_view phrase, std::string_view pinyin) const {
    const auto &s = shards_[shardFor(phrase)];
    return s && s->isSuppressed(phrase, pinyin);
}
double LearningSnapshot::boostAt(std::string_view phrase, std::string_view pinyin, std::int64_t now) const {
    const auto &s = shards_[shardFor(phrase)];
    return s ? s->boostAt(phrase, pinyin, now) : 0;
}
std::int64_t LearningSnapshot::lastSelectedMs(std::string_view phrase, std::string_view pinyin) const {
    const auto &s = shards_[shardFor(phrase)];
    return s ? s->lastSelectedMs(phrase, pinyin) : 0;
}
double LearningSnapshot::contextBoost(std::string_view phrase, std::string_view pinyin,
    std::string_view before, std::string_view after, std::int64_t now) const {
    const auto &s = shards_[shardFor(phrase)];
    return s ? s->contextBoost(phrase, pinyin, before, after, now) : 0;
}
void LearningSnapshot::recordSelection(std::string_view phrase, std::string_view pinyin,
    std::string_view before, std::string_view after, std::int64_t now) {
    auto &s = mutableShard(phrase);
    const auto count = s.entryCount();
    s.recordSelection(phrase, pinyin, before, after, now);
    finishMutation(count, phrase);
}
void LearningSnapshot::recordNegativeFeedback(std::string_view phrase, std::string_view pinyin) {
    auto &s = mutableShard(phrase);
    const auto count = s.entryCount();
    s.recordNegativeFeedback(phrase, pinyin);
    finishMutation(count, phrase);
}
void LearningSnapshot::recordSuppression(std::string_view phrase, std::string_view pinyin) {
    auto &s = mutableShard(phrase);
    const auto count = s.entryCount();
    s.recordSuppression(phrase, pinyin);
    finishMutation(count, phrase);
}
const std::vector<LearningEntry> &LearningSnapshot::entries() const {
    std::call_once(entriesCache_->initialized, [this] {
        auto &entries = entriesCache_->entries;
        entries.reserve(entryCount_);
        for (const auto &s : shards_) if (s) {
            const auto &part = s->entries();
            entries.insert(entries.end(), part.begin(), part.end());
        }
    });
    return entriesCache_->entries;
}

std::vector<RecalledCandidate> LearningSnapshot::recallCandidates(std::string_view input,
    std::int64_t now, std::string_view before, std::string_view after) const {
    if (input.empty()) return {};
    std::vector<RecalledCandidate> result, prefixes;
    std::unordered_set<std::string> seen;
    const auto canonical = PinyinMatchPolicy::canonical(input);
    const bool abbreviated = PinyinMatchPolicy::isAbbreviationInput(input);
    auto append = [&](std::string_view phrase, std::string_view pinyin,
                      std::vector<RecalledCandidate> &destination) {
        const auto key = candidateOrderKey(phrase, pinyin);
        if (seen.contains(key) || isSuppressed(phrase, pinyin) ||
            !hasPositiveFrequency(phrase, pinyin)) return;
        if (&destination == &result) seen.insert(key);
        destination.push_back({std::string(phrase), std::string(pinyin),
            boostAt(phrase, pinyin, now), contextBoost(phrase, pinyin, before, after, now)});
    };
    using Prefix = std::tuple<std::string, std::string, std::string>;
    struct Cursor { const std::vector<Prefix> *entries; std::size_t offset; };
    auto later = [](const Cursor &a, const Cursor &b) {
        return (*a.entries)[a.offset] > (*b.entries)[b.offset];
    };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> ranges(later);
    for (const auto &shard : shards_) if (shard) {
        const auto &index = shard->ensureRecallIndex();
        const auto exact = index.canonicalIndex.find(canonical);
        if (!canonical.empty() && exact != index.canonicalIndex.end()) {
            for (const auto &[phrase, pinyin] : exact->second) append(phrase, pinyin, result);
        }
        if (abbreviated) {
            // Each zh/ch/sh can be either one digraph initial or two
            // syllable initials. Search only prefixes present in this shard's
            // sorted abbreviation keys, rather than enumerating 2^N strings.
            std::string key;
            const auto visit = [&](auto self, std::size_t offset) -> void {
                const auto found = std::lower_bound(index.abbreviationKeys.begin(), index.abbreviationKeys.end(), key);
                if (found == index.abbreviationKeys.end() || !found->starts_with(key)) return;
                if (offset == input.size()) {
                    const auto abbr = index.abbrIndex.find(key);
                    if (abbr != index.abbrIndex.end()) {
                        for (const auto &[phrase, pinyin] : abbr->second) {
                            if (key == input || PinyinMatchPolicy::initialsMatch(input, pinyin)) append(phrase, pinyin, result);
                        }
                    }
                    return;
                }
                key.push_back(input[offset]);
                self(self, offset + 1);
                if (offset + 1 < input.size() && input[offset + 1] == 'h' &&
                    (input[offset] == 'z' || input[offset] == 'c' || input[offset] == 's')) self(self, offset + 2);
                key.pop_back();
            };
            visit(visit, 0);
        }
        if (canonical.size() >= 3) {
            const auto begin = std::lower_bound(index.prefixIndex.begin(), index.prefixIndex.end(), canonical,
                [](const auto &item, const std::string &key) { return std::get<0>(item) < key; });
            if (begin != index.prefixIndex.end() && std::get<0>(*begin).starts_with(canonical)) {
                ranges.push({&index.prefixIndex, static_cast<std::size_t>(begin - index.prefixIndex.begin())});
            }
        }
    }
    // Merge sorted ranges before consuming the original global 256-entry
    // budget, so hash partitioning cannot starve lexically earlier candidates.
    for (std::size_t scanned = 0; scanned < 256 && !ranges.empty(); ++scanned) {
        auto cursor = ranges.top();
        ranges.pop();
        const auto &[key, phrase, pinyin] = (*cursor.entries)[cursor.offset];
        append(phrase, pinyin, prefixes);
        ++cursor.offset;
        if (cursor.offset < cursor.entries->size() &&
            std::get<0>((*cursor.entries)[cursor.offset]).starts_with(canonical)) ranges.push(cursor);
    }
    std::sort(prefixes.begin(), prefixes.end(), [](const auto &a, const auto &b) {
        const double aScore = a.learningBoost + a.contextBoost;
        const double bScore = b.learningBoost + b.contextBoost;
        const bool aFin = std::isfinite(aScore);
        const bool bFin = std::isfinite(bScore);
        if (aFin != bFin) return aFin;
        if (aFin && aScore != bScore) return aScore > bScore;
        if (a.phrase != b.phrase) return a.phrase < b.phrase;
        return a.pinyin < b.pinyin;
    });
    if (prefixes.size() > 16) prefixes.resize(16);
    for (auto &candidate : prefixes) {
        if (seen.insert(candidateOrderKey(candidate.phrase, candidate.pinyin)).second)
            result.push_back(std::move(candidate));
    }
    return result;
}
std::vector<RecalledEnglishWord> LearningSnapshot::recallEnglishWords(std::string_view prefix,
    std::size_t limit, std::int64_t now) const {
    std::vector<RecalledEnglishWord> result;
    const std::size_t shardQueryLimit = std::max(limit, std::size_t{16});
    for (const auto &s : shards_) if (s) {
        auto part = s->recallEnglishWords(prefix, shardQueryLimit, now);
        result.insert(result.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
    std::unordered_map<std::string, RecalledEnglishWord> uniqueWords;
    for (auto &w : result) {
        auto it = uniqueWords.find(w.text);
        if (it == uniqueWords.end()) {
            uniqueWords.emplace(w.text, std::move(w));
        } else {
            it->second.frequency += w.frequency;
            if (w.lastSelectedMs > it->second.lastSelectedMs) {
                it->second.lastSelectedMs = w.lastSelectedMs;
                it->second.baseBoost = w.baseBoost;
            }
        }
    }
    result.clear();
    result.reserve(uniqueWords.size());
    for (auto &pair : uniqueWords) {
        result.push_back(std::move(pair.second));
    }
    std::int64_t latestMs = 0;
    double latestBoost = 0.0;
    for (const auto &item : result) {
        if (item.lastSelectedMs > latestMs) {
            latestMs = item.lastSelectedMs;
            latestBoost = std::isfinite(item.baseBoost) ? std::max(0.0, item.baseBoost) : 0.0;
        } else if (item.lastSelectedMs == latestMs) {
            const double boost = std::isfinite(item.baseBoost) ? std::max(0.0, item.baseBoost) : 0.0;
            latestBoost = std::max(latestBoost, boost);
        }
    }
    constexpr double kDormantHalfLifeMs = 12.0 * 3600.0 * 1000.0;
    constexpr double kDormantTauMs = kDormantHalfLifeMs / 0.6931471805599453;
    const auto lowerPrefix = toLowerAscii(prefix);
    for (auto &item : result) {
        double effectiveBoost = item.baseBoost;
        if (latestMs > 0 && item.lastSelectedMs > 0 && item.lastSelectedMs < latestMs) {
            const auto ageSinceLatest = latestMs - item.lastSelectedMs;
            double dormant = std::exp(-static_cast<double>(ageSinceLatest) / kDormantTauMs);
            if (latestBoost >= 1.8) {
                const double discount = 1.0 - 0.30 * std::min(1.0, (latestBoost - 1.8) / (3.9 - 1.8));
                dormant = std::min(dormant, discount);
            }
            effectiveBoost = item.baseBoost * dormant;
        }
        const auto lower = toLowerAscii(item.text);
        double completeness = 0.0;
        if (!lower.empty()) {
            completeness = static_cast<double>(lowerPrefix.size()) /
                           static_cast<double>(lower.size());
        }
        if (lower == lowerPrefix) {
            completeness += 1.0;
        }
        item.score = effectiveBoost + completeness;
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        const bool aFin = std::isfinite(a.score);
        const bool bFin = std::isfinite(b.score);
        if (aFin != bFin) return aFin;
        if (aFin && a.score != b.score) return a.score > b.score;
        if (a.frequency != b.frequency) return a.frequency > b.frequency;
        if (a.lastSelectedMs != b.lastSelectedMs) return a.lastSelectedMs > b.lastSelectedMs;
        return a.text < b.text;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

} // namespace modernime::core
