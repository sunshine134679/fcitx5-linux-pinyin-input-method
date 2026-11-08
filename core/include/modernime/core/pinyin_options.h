#pragma once

#include <array>

namespace modernime::core {

struct FuzzyPinyinOptions final {
    bool zZh = true;
    bool cCh = true;
    bool sSh = true;
    bool lN = true;
    bool enEng = true;
    bool inIng = true;
    bool operator==(const FuzzyPinyinOptions &) const = default;
};

struct FuzzyPinyinOption final {
    const char *key;
    const char *label;
    bool FuzzyPinyinOptions::*member;
};

inline constexpr std::array kFuzzyPinyinOptions{
    FuzzyPinyinOption{"pinyin.fuzzy_z_zh", "z / zh", &FuzzyPinyinOptions::zZh},
    FuzzyPinyinOption{"pinyin.fuzzy_c_ch", "c / ch", &FuzzyPinyinOptions::cCh},
    FuzzyPinyinOption{"pinyin.fuzzy_s_sh", "s / sh", &FuzzyPinyinOptions::sSh},
    FuzzyPinyinOption{"pinyin.fuzzy_l_n", "l / n", &FuzzyPinyinOptions::lN},
    FuzzyPinyinOption{"pinyin.fuzzy_en_eng", "en / eng", &FuzzyPinyinOptions::enEng},
    FuzzyPinyinOption{"pinyin.fuzzy_in_ing", "in / ing", &FuzzyPinyinOptions::inIng},
};

} // namespace modernime::core
