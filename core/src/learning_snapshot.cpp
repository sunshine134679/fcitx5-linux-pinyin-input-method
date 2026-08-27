#include "modernime/core/learning_snapshot.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace modernime::core {

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

double LearningSnapshot::boostAt(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) const {
    const auto *candidate = entry(phrase, pinyin, contextBefore, contextAfter);
    if (candidate == nullptr) {
        return 0.0;
    }
    const auto ageMs = std::max<std::int64_t>(
        0, nowMs - candidate->lastSelectedMs);
    constexpr double halfLifeMs = 30.0 * 24.0 * 60.0 * 60.0 * 1000.0;
    const double recency = std::exp(-static_cast<double>(ageMs) / halfLifeMs);
    const double frequency = std::min(
        2.0, 0.65 * std::log1p(static_cast<double>(candidate->frequency)));
    const double recent = std::min(1.0, 0.90 * recency);
    const double penalty = std::min(
        2.0, 0.75 * std::log1p(
                 static_cast<double>(candidate->negativeFeedback)));
    return std::clamp(frequency + recent - penalty, -2.0, 3.5);
}

double LearningSnapshot::contextBoost(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter) const {
    if (contextBefore.empty() && contextAfter.empty()) {
        return 0.0;
    }
    const auto normalized = normalizePinyin(pinyin);
    for (const auto &candidate : entries_) {
        if (candidate.phrase != phrase || candidate.pinyin != normalized) {
            continue;
        }
        if (candidate.contextBefore == contextBefore &&
            candidate.contextAfter == contextAfter &&
            !contextBefore.empty() && !contextAfter.empty()) {
            return 0.8;
        }
        if ((candidate.contextBefore == contextBefore &&
             !contextBefore.empty() && candidate.contextAfter.empty()) ||
            (candidate.contextAfter == contextAfter &&
             !contextAfter.empty() && candidate.contextBefore.empty())) {
            return 0.4;
        }
    }
    return 0.0;
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
    auto *candidate = mutableEntry(phrase, pinyin, contextBefore, contextAfter);
    ++candidate->frequency;
    candidate->lastSelectedMs = nowMs;
}

void LearningSnapshot::recordNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    auto *candidate = mutableEntry(phrase, pinyin, {}, {});
    ++candidate->negativeFeedback;
}

} // namespace modernime::core
