#include "modernime/pinyin/candidate_prefixes.h"
#include "modernime/pinyin/recent_choice.h"
#include "modernime/core/learning_snapshot.h"
#include <algorithm>
#include <optional>

namespace modernime::pinyin {
namespace {
std::size_t utf8CharacterCount(std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](const char character) {
            return (static_cast<unsigned char>(character) & 0xC0U) != 0x80U;
        }));
}

std::optional<std::string> utf8Prefix(std::string_view text,
                                      std::size_t characterCount) {
    std::size_t seen = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if ((static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
            continue;
        }
        if (seen == characterCount) {
            return std::string(text.substr(0, offset));
        }
        ++seen;
    }
    if (seen == characterCount) {
        return std::string(text);
    }
    return std::nullopt;
}

std::size_t pinyinSyllableCount(std::string_view fullPinyin) {
    if (fullPinyin.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(
               std::count(fullPinyin.begin(), fullPinyin.end(), '\'')) +
           1;
}

std::optional<std::string> pinyinPrefix(std::string_view fullPinyin,
                                        std::size_t syllableCount) {
    std::size_t separators = 0;
    for (std::size_t offset = 0; offset < fullPinyin.size(); ++offset) {
        if (fullPinyin[offset] != '\'') {
            continue;
        }
        ++separators;
        if (separators == syllableCount) {
            return std::string(fullPinyin.substr(0, offset));
        }
    }
    return std::nullopt;
}

std::optional<core::CandidateItem> derivedPrefix(
    const core::CandidateItem &bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds,
    std::string_view rawPinyin, std::size_t syllableCount,
    const std::vector<core::CandidateItem> &partialCandidates,
    const core::LearningSnapshot *learning, std::int64_t nowMs) {
    if (syllableCount == 0 || syllablePrefixEnds.size() < syllableCount ||
        syllablePrefixEnds[syllableCount - 1] >= rawPinyin.size()) {
        return std::nullopt;
    }

    const auto totalSyllables =
        pinyinSyllableCount(bestFullSentence.fullPinyin);
    if (totalSyllables != syllablePrefixEnds.size() + 1 ||
        utf8CharacterCount(bestFullSentence.text) != totalSyllables) {
        return std::nullopt;
    }

    const auto text = utf8Prefix(bestFullSentence.text, syllableCount);
    const auto fullPinyin =
        pinyinPrefix(bestFullSentence.fullPinyin, syllableCount);
    if (!text.has_value() || !fullPinyin.has_value()) {
        return std::nullopt;
    }

    core::CandidateItem result;
    result.text = *text;
    result.fullPinyin = *fullPinyin;
    result.consumedInputBytes = syllablePrefixEnds[syllableCount - 1];
    // The parent supplies segmentation, never the prefix's personal evidence.
    for (const auto &candidate : partialCandidates) {
        if (candidate.text == result.text &&
            candidate.fullPinyin == result.fullPinyin &&
            candidate.consumedInputBytes == result.consumedInputBytes) {
            return candidate;
        }
    }
    if (learning != nullptr) {
        result.learningBoost = learning->boostAt(result.text, result.fullPinyin, nowMs);
        if (learning->hasPositiveFrequency(result.text, result.fullPinyin)) {
            result.source = core::CandidateSource::Learned;
        }
    }
    return result;
}

std::optional<std::string> matchingBestTextPrefix(
    const core::CandidateItem &candidate,
    const core::CandidateItem &bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds) {
    const auto syllableCount = pinyinSyllableCount(candidate.fullPinyin);
    if (syllableCount == 0 || syllableCount > syllablePrefixEnds.size() ||
        candidate.consumedInputBytes !=
            syllablePrefixEnds[syllableCount - 1]) {
        return std::nullopt;
    }
    const auto expectedPinyin =
        pinyinPrefix(bestFullSentence.fullPinyin, syllableCount);
    if (!expectedPinyin.has_value() ||
        candidate.fullPinyin != *expectedPinyin) {
        return std::nullopt;
    }
    return utf8Prefix(bestFullSentence.text, syllableCount);
}

} // namespace

std::vector<core::CandidateItem> buildPrefixEvidence(
    const core::CandidateItem &best, const std::vector<std::size_t> &ends,
    std::string_view input, const std::vector<core::CandidateItem> &partial,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    const RecentChoices *recentChoices) {
    std::vector<core::CandidateItem> result;
    for (const auto count : {std::size_t{3}, std::size_t{2}}) {
        auto item = derivedPrefix(best, ends, input, count, partial, learning, nowMs);
        if (item && (learning == nullptr || !learning->isSuppressed(item->text, item->fullPinyin))) {
            if (recentChoices != nullptr && item->learningBoost >= 1.8) {
                item->recentChoiceBonus = recentChoices->bonus(input, item->text, item->fullPinyin, true);
            }
            result.push_back(std::move(*item));
        }
    }
    return result;
}

bool isMatchingSentenceHomophone(const core::CandidateItem &candidate,
    const core::CandidateItem &best, const std::vector<std::size_t> &ends) {
    const auto expected = matchingBestTextPrefix(candidate, best, ends);
    return expected && candidate.text != *expected;
}
} // namespace modernime::pinyin
