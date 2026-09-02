#include "modernime/pinyin/candidate_mixer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>

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
    std::string_view rawPinyin, std::size_t syllableCount) {
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

    auto result = bestFullSentence;
    result.text = *text;
    result.fullPinyin = *fullPinyin;
    result.consumedInputBytes = syllablePrefixEnds[syllableCount - 1];
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

std::vector<core::CandidateItem> mixCandidateItems(
    const std::vector<core::CandidateItem> &fullCandidates,
    const std::vector<core::CandidateItem> &partialCandidates,
    const core::CandidateItem *bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds,
    std::string_view rawPinyin) {
    if (bestFullSentence == nullptr || fullCandidates.empty() ||
        syllablePrefixEnds.size() < 2) {
        auto result = fullCandidates;
        result.insert(result.end(), partialCandidates.begin(),
                      partialCandidates.end());
        return result;
    }

    std::vector<core::CandidateItem> result;
    result.reserve(fullCandidates.size() + partialCandidates.size() + 2);
    std::unordered_set<std::string> seenText;
    seenText.reserve(result.capacity());
    const auto append = [&result, &seenText](const core::CandidateItem &item) {
        if (item.text.empty() || !seenText.emplace(item.text).second) {
            return false;
        }
        result.push_back(item);
        return true;
    };

    append(*bestFullSentence);
    const auto threeSyllable = derivedPrefix(
        *bestFullSentence, syllablePrefixEnds, rawPinyin, 3);
    if (threeSyllable.has_value()) {
        append(*threeSyllable);
    }
    const core::CandidateItem *runnerUpFullSentence = nullptr;
    for (const auto &candidate : fullCandidates) {
        if (!seenText.contains(candidate.text)) {
            runnerUpFullSentence = &candidate;
            append(candidate);
            break;
        }
    }
    const auto twoSyllable = derivedPrefix(
        *bestFullSentence, syllablePrefixEnds, rawPinyin, 2);
    if (twoSyllable.has_value()) {
        append(*twoSyllable);
    }

    const core::CandidateItem *homophone = nullptr;
    for (const auto &candidate : partialCandidates) {
        const auto bestTextPrefix = matchingBestTextPrefix(
            candidate, *bestFullSentence, syllablePrefixEnds);
        if (!bestTextPrefix.has_value() ||
            candidate.text == *bestTextPrefix ||
            seenText.contains(candidate.text)) {
            continue;
        }
        homophone = &candidate;
        append(candidate);
        break;
    }

    for (const auto &candidate : fullCandidates) {
        if (&candidate != runnerUpFullSentence) {
            append(candidate);
        }
    }
    for (const auto &candidate : partialCandidates) {
        if (&candidate == homophone || seenText.contains(candidate.text)) {
            continue;
        }
        append(candidate);
    }
    return result;
}

} // namespace modernime::pinyin
