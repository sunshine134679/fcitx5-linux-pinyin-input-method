#include "modernime/core/pinyin_match.h"

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

int PinyinMatchPolicy::priority(std::string_view userInput,
                                std::string_view fullPinyin) {
    if (exactInputMatch(userInput, fullPinyin)) {
        return 2;
    }
    if (isAbbreviationInput(userInput) &&
        abbreviationKey(fullPinyin) == userInput) {
        return 1;
    }
    const auto input = canonical(userInput);
    const auto candidate = canonical(fullPinyin);
    if (!candidate.empty() && candidate.size() < input.size() &&
        input.substr(0, candidate.size()) == candidate) {
        return -1;
    }
    return 0;
}

} // namespace modernime::core
