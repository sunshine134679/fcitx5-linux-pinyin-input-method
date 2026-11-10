#pragma once

#include "modernime/core/pinyin_options.h"
#include <string>

namespace modernime::core {
struct CandidatePin final {
    PinyinScheme scheme = PinyinScheme::Full;
    std::string input;
    std::string text;
    std::string fullPinyin;
    bool operator==(const CandidatePin &) const = default;
};
} // namespace modernime::core
