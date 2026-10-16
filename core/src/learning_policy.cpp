#include "modernime/core/learning_policy.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_snapshot.h"
#include "modernime/core/pinyin_match.h"

#include <cctype>

namespace modernime::core {

bool isProtectedCoreDefault(std::string_view text, std::string_view pinyin) {
    const auto can = PinyinMatchPolicy::canonical(pinyin);
    if (text == "是" && can == "shi") return true;
    if (text == "的" && can == "de") return true;
    if (text == "你" && can == "ni") return true;
    if (text == "好" && can == "hao") return true;
    if (text == "吗" && can == "ma") return true;
    if (text == "和" && can == "he") return true;
    if (text == "在" && can == "zai") return true;
    if (text == "有" && can == "you") return true;
    if (text == "了" && can == "le") return true;
    if (text == "不" && can == "bu") return true;
    if (text == "我" && can == "wo") return true;

    if (text == "这个" && can == "zhege") return true;
    if (text == "不是" && can == "bushi") return true;
    if (text == "好的" && can == "haode") return true;
    if (text == "可以" && can == "keyi") return true;

    return false;
}

CommitLearningDecision decideCommitLearning(const CommitEvent &event,
                                            bool hasPositiveFrequency) {
    if (event.role == CommitCandidateRole::Command) {
        return CommitLearningDecision::IgnoreCommand;
    }

    if (event.role == CommitCandidateRole::Raw) {
        if (PinyinMatchPolicy::isValidPinyinSequence(event.text) &&
            !EnglishDictionary::isEnglishWord(event.text)) {
            return CommitLearningDecision::IgnoreRawPinyin;
        }
    }

    const auto pinyinKey = event.fullPinyin.empty() ? event.text : event.fullPinyin;

    if (event.role == CommitCandidateRole::Chinese &&
        event.candidateIndex == 0 &&
        !event.isDeviation &&
        !event.isPartial &&
        event.contextBefore.empty() &&
        event.contextAfter.empty() &&
        isProtectedCoreDefault(event.text, pinyinKey)) {
        if (!hasPositiveFrequency) {
            return CommitLearningDecision::IgnoreCoreDefault;
        }
    }

    return CommitLearningDecision::Record;
}

std::string canonicalEnglishSelectionWord(std::string_view phrase,
                                          std::string_view pinyin) {
    if (phrase.empty() || phrase.size() > 64) {
        return {};
    }
    std::string lower;
    lower.reserve(phrase.size());
    bool has_alpha = false;
    for (const unsigned char c : phrase) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            has_alpha = true;
            lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
        } else if (c == '_' || c == '-') {
            lower.push_back(static_cast<char>(c));
        } else {
            return {};
        }
    }
    return has_alpha && normalizePinyin(pinyin) == lower ? lower : std::string{};
}

} // namespace modernime::core
