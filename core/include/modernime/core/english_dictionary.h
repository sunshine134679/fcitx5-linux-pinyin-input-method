#pragma once

#include <string_view>
#include <vector>

namespace modernime::core {

class EnglishDictionary final {
public:
    static bool isEnglishWord(std::string_view word) noexcept;
    static std::vector<std::string_view> predictWords(std::string_view prefix,
                                                      std::size_t maxCount = 3) noexcept;
};

} // namespace modernime::core
