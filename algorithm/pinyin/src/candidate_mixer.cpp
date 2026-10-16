#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/candidate_ranker.h"
#include "modernime/pinyin/candidate_prefixes.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace modernime::pinyin {
std::vector<core::CandidateItem> mixCandidateItems(
    const std::vector<core::CandidateItem> &fullCandidates,
    const std::vector<core::CandidateItem> &partialCandidates,
    const core::CandidateItem *bestFullSentence,
    const std::vector<std::size_t> &syllablePrefixEnds,
    std::string_view rawPinyin, const std::vector<core::CandidateItem> *prefixEvidence) {
    std::vector<core::CandidateItem> result;
    result.reserve(fullCandidates.size() + partialCandidates.size() + 2);
    std::unordered_map<core::CandidateIdentity, std::size_t> seen;
    seen.reserve(result.capacity());
    const auto append = [&result, &seen, rawPinyinSize = rawPinyin.size()](const core::CandidateItem &item) {
        if (item.text.empty()) {
            return false;
        }
        const auto id = core::candidateIdentity(item, rawPinyinSize);
        auto it = seen.find(id);
        if (it != seen.end()) {
            auto &existing = result[it->second];
            for (auto src : item.origins) {
                if (std::find(existing.origins.begin(), existing.origins.end(), src) == existing.origins.end()) {
                    existing.origins.push_back(src);
                }
            }
            if (std::find(existing.origins.begin(), existing.origins.end(), item.source) == existing.origins.end()) {
                existing.origins.push_back(item.source);
            }
            return false;
        }
        seen[id] = result.size();
        result.push_back(item);
        if (result.back().origins.empty()) {
            result.back().origins.push_back(result.back().source);
        }
        return true;
    };

    if (bestFullSentence == nullptr || fullCandidates.empty() ||
        syllablePrefixEnds.size() < 2) {
        for (const auto &item : fullCandidates) append(item);
        for (const auto &item : partialCandidates) append(item);
        return result;
    }

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
        const auto id = core::candidateIdentity(candidate, rawPinyin.size());
        if (!seen.contains(id)) {
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
        const auto id = core::candidateIdentity(candidate, rawPinyin.size());
        if (!isMatchingSentenceHomophone(candidate, *bestFullSentence, syllablePrefixEnds) ||
            seen.contains(id) ||
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
        rawFallback.fullPinyin = std::string(rawPinyin);
        rawFallback.source = core::CandidateSource::Raw;
        rawFallback.commitRole = core::CommitCandidateRole::Raw;
        rawFallback.origins = {rawFallback.source};
        rawFallback.consumedInputBytes = rawPinyin.size();
        append(rawFallback);
    }

    for (const auto &candidate : fullCandidates) {
        if (&candidate != runnerUpFullSentence) {
            append(candidate);
        }
    }
    for (const auto &candidate : partialCandidates) {
        if (&candidate != homophone) {
            append(candidate);
        }
    }
    return result;
}

} // namespace modernime::pinyin
