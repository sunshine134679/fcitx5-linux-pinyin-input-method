#pragma once

#include <string_view>
#include <vector>

namespace modernime::core {

class EnglishDictionary final {
public:
    // Case-insensitive built-in lexicon membership, without the vowel and
    // short-code restrictions used for Chinese/English arbitration.
    static bool containsWord(std::string_view word) noexcept;
    static bool isEnglishWord(std::string_view word) noexcept;
    static bool isCommonWord(std::string_view word) noexcept;
    static std::vector<std::string_view> predictWords(std::string_view prefix,
                                                      std::size_t maxCount = 3) noexcept;
    static double wordPopularityWeight(std::string_view word) noexcept;
    static bool isModernTechWord(std::string_view word) noexcept;
};

} // namespace modernime::core
