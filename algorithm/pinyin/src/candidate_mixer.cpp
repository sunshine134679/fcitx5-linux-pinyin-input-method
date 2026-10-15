#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/candidate_ranker.h"
#include "modernime/pinyin/candidate_prefixes.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>

namespace modernime::pinyin {
std::vector<core::CandidateItem> mixCandidateItems(
    const std::vector<core::CandidateItem> &fullCandidates,
    const std::vector<core::CandidateItem> &partialCandidates,
    const core::CandidateItem *bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds,
    std::string_view rawPinyin, const std::vector<core::CandidateItem> *prefixEvidence) {
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

    const auto derived = prefixEvidence != nullptr ? *prefixEvidence :
        buildPrefixEvidence(*bestFullSentence, syllablePrefixEnds, rawPinyin, partialCandidates);
    const core::CandidateItem *threeSyllable = nullptr, *twoSyllable = nullptr;
    for (const auto &item : derived) {
        const auto syllables = std::count(item.fullPinyin.begin(), item.fullPinyin.end(), '\'') + 1;
        if (syllables == 3) threeSyllable = &item;
        if (syllables == 2) twoSyllable = &item;
    }
    const core::CandidateItem *personalPrefix = nullptr;
    const auto considerPersonal = [&](const core::CandidateItem &item) {
        if (item.learningBoost < 1.8 || !item.consumedInputBytes ||
            item.consumedInputBytes >= rawPinyin.size()) {
            return;
        }
        const auto preference = [](const core::CandidateItem &candidate) {
            return core::computeAdaptiveLearning(candidate.learningBoost) + candidate.recentChoiceBonus;
        };
        if (personalPrefix == nullptr || preference(item) > preference(*personalPrefix)) {
            personalPrefix = &item;
        }
    };
    for (const auto &item : partialCandidates) considerPersonal(item);
    if (threeSyllable) considerPersonal(*threeSyllable);
    if (twoSyllable) considerPersonal(*twoSyllable);

    append(*bestFullSentence);
    if (personalPrefix != nullptr) {
        append(*personalPrefix);
    } else if (threeSyllable) {
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
    if (personalPrefix != nullptr && threeSyllable) {
        append(*threeSyllable);
    }
    if (twoSyllable != nullptr) {
        append(*twoSyllable);
    }

    const core::CandidateItem *homophone = nullptr;
    for (const auto &candidate : partialCandidates) {
        if (!isMatchingSentenceHomophone(candidate, *bestFullSentence, syllablePrefixEnds) ||
            seenText.contains(candidate.text) ||
            core::CommonCharacters::containsRare(candidate.text) ||
            !core::CommonCharacters::isLevel1(
                core::CommonCharacters::firstCodepoint(candidate.text))) {
            continue;
        }
        homophone = &candidate;
        append(candidate);
        break;
    }

    const bool enforcesTopFiveFullSentenceQuota =
        runnerUpFullSentence != nullptr && threeSyllable != nullptr &&
        twoSyllable != nullptr;
    if (homophone == nullptr && enforcesTopFiveFullSentenceQuota &&
        result.size() < 5) {
        core::CandidateItem rawFallback;
        rawFallback.text = rawPinyin;
        rawFallback.source = core::CandidateSource::Raw;
        rawFallback.consumedInputBytes = rawPinyin.size();
        append(rawFallback);
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
