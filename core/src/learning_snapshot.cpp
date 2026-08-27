#include "modernime/core/learning_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace modernime::core {

namespace {

std::int64_t saturatingIncrement(std::int64_t value) {
    if (value < 0 || value == std::numeric_limits<std::int64_t>::max()) {
        return value < 0 ? 1 : value;
    }
    return value + 1;
}

std::int64_t saturatingAddNonNegative(std::int64_t left,
                                      std::int64_t right) {
    left = std::max<std::int64_t>(0, left);
    right = std::max<std::int64_t>(0, right);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (right > maximum - left) {
        return maximum;
    }
    return left + right;
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

LearningSnapshot::LearningSnapshot(std::vector<LearningEntry> entries)
    : entries_(std::move(entries)) {}

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

double LearningSnapshot::boostAt(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) const {
    const auto normalized = normalizePinyin(pinyin);
    if (isSuppressed(phrase, normalized)) {
        return 0.0;
    }
    const LearningEntry *candidate = nullptr;
    const LearningEntry *base = nullptr;
    for (const auto &item : entries_) {
        if (item.phrase != phrase || item.pinyin != normalized) {
            continue;
        }
        if (item.contextBefore == contextBefore &&
            item.contextAfter == contextAfter) {
            candidate = &item;
        } else if (item.contextBefore.empty() && item.contextAfter.empty()) {
            base = &item;
        }
    }
    if (candidate == nullptr) {
        candidate = base;
    }
    if (candidate == nullptr) {
        return 0.0;
    }
    const double ageMs = std::max(
        0.0, static_cast<double>(nowMs) -
                 static_cast<double>(candidate->lastSelectedMs));
    constexpr double halfLifeMs = 30.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    const double recency = std::exp(-ageMs / halfLifeMs);
    const auto frequencyCount = std::max<std::int64_t>(0, candidate->frequency);
    const double frequency = std::min(
        2.0, 0.65 * std::log1p(static_cast<double>(frequencyCount)));
    const double recent = candidate->frequency > 0
                              ? std::min(1.0, 0.90 * recency)
                              : 0.0;
    auto negativeFeedback = std::max<std::int64_t>(
        0, candidate->negativeFeedback);
    // A global deletion/negative-feedback event must still apply when a
    // more-specific contextual selection exists for the same candidate.
    if (candidate != base && base != nullptr) {
        negativeFeedback = saturatingAddNonNegative(
            negativeFeedback, base->negativeFeedback);
    }
    negativeFeedback = std::max<std::int64_t>(0, negativeFeedback);
    const double penalty = std::min(
        2.0, 0.75 * std::log1p(static_cast<double>(negativeFeedback)));
    return std::clamp(frequency + recent - penalty, -2.0, 3.5);
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
}

void LearningSnapshot::recordNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    auto *candidate = mutableEntry(phrase, pinyin, {}, {});
    candidate->negativeFeedback =
        saturatingIncrement(candidate->negativeFeedback);
}

void LearningSnapshot::recordSuppression(std::string_view phrase,
                                         std::string_view pinyin) {
    auto *candidate = mutableEntry(phrase, pinyin, {}, {});
    candidate->suppressed = true;
    candidate->negativeFeedback =
        saturatingIncrement(candidate->negativeFeedback);
}

} // namespace modernime::core
