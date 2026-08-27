#pragma once

#include <string>
#include <string_view>

namespace modernime::core {

class PinyinMatchPolicy final {
public:
    static std::string canonical(std::string_view fullPinyin);
    static std::string abbreviationKey(std::string_view fullPinyin);
    static bool validComposition(std::string_view userInput);
    static bool isAbbreviationInput(std::string_view userInput);
    static bool exactInputMatch(std::string_view userInput,
                                std::string_view fullPinyin);
    static int priority(std::string_view userInput,
                        std::string_view fullPinyin);
};

} // namespace modernime::core
