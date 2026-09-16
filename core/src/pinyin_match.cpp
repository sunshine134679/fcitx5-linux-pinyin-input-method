#include "modernime/core/pinyin_match.h"

#include <array>
#include <cctype>

namespace modernime::core {

std::string PinyinMatchPolicy::canonical(std::string_view fullPinyin) {
    std::string result;
    result.reserve(fullPinyin.size());
    for (const char character : fullPinyin) {
        if (character != '\'') {
            result.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(character))));
        }
    }
    return result;
}

std::string PinyinMatchPolicy::abbreviationKey(std::string_view fullPinyin) {
    std::string result;
    bool syllableStart = true;
    for (const char character : fullPinyin) {
        if (character == '\'') {
            syllableStart = true;
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(character))) {
            continue;
        }
        if (syllableStart) {
            result.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(character))));
        }
        syllableStart = false;
    }
    return result;
}

bool PinyinMatchPolicy::validComposition(std::string_view userInput) {
    if (userInput.empty()) {
        return false;
    }
    bool hasLetter = false;
    bool separator = false;
    for (const char character : userInput) {
        if (character >= 'a' && character <= 'z') {
            hasLetter = true;
            separator = false;
            continue;
        }
        if (character != '\'' || !hasLetter || separator) {
            return false;
        }
        separator = true;
    }
    return hasLetter;
}

bool PinyinMatchPolicy::isAbbreviationInput(std::string_view userInput) {
    if (userInput.size() < 2) {
        return false;
    }
    for (const char character : userInput) {
        if (!std::islower(static_cast<unsigned char>(character))) {
            return false;
        }
    }
    return true;
}

bool PinyinMatchPolicy::exactInputMatch(std::string_view userInput,
                                        std::string_view fullPinyin) {
    return canonical(fullPinyin) == canonical(userInput);
}

bool PinyinMatchPolicy::trustedShortAbbreviationMatch(
    std::string_view userInput, std::string_view fullPinyin,
    std::string_view text) {
    if (userInput.size() != 3 || !isAbbreviationInput(userInput) ||
        abbreviationKey(fullPinyin) != userInput) {
        return false;
    }

    static constexpr std::array commonPhrases{
        std::string_view{"为什么"}, std::string_view{"没什么"},
        std::string_view{"怎么样"}, std::string_view{"怎么办"},
        std::string_view{"不知道"}, std::string_view{"没问题"},
        std::string_view{"对不起"}, std::string_view{"没关系"},
        std::string_view{"谢谢你"}, std::string_view{"我知道"},
    };
    for (const auto phrase : commonPhrases) {
        if (text == phrase) {
            return true;
        }
    }
    return false;
}

int PinyinMatchPolicy::priority(std::string_view userInput,
                                std::string_view fullPinyin) {
    // 原实现经 exactInputMatch 兜底各调一次 canonical（每候选四次字符串
    // 分配）；ranker 对每个候选调用本函数，这里直接算一次复用。
    const auto input = canonical(userInput);
    const auto candidate = canonical(fullPinyin);
    if (candidate == input) {
        return 2;
    }
    // isAbbreviationInput 只依赖 userInput，对所有候选结果相同，
    // 先短路再做依赖候选的 abbreviationKey。
    if (isAbbreviationInput(userInput) &&
        abbreviationKey(fullPinyin) == userInput) {
        return 1;
    }
    if (!candidate.empty() && candidate.size() < input.size() &&
        input.substr(0, candidate.size()) == candidate) {
        return -1;
    }
    return 0;
}

} // namespace modernime::core
