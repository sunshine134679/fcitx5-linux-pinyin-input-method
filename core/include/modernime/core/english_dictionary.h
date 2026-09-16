#pragma once

#include <string_view>

namespace modernime::core {

class EnglishDictionary final {
public:
    static bool isEnglishWord(std::string_view word) noexcept;
};

} // namespace modernime::core
