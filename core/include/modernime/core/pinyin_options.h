#pragma once

#include <array>

namespace modernime::core {

enum class PinyinScheme { Full, Xiaohe, Ziranma };

struct PinyinSchemeOption final {
    PinyinScheme scheme;
    const char *key;
    const char *label;
};

inline constexpr std::array kPinyinSchemes{
    PinyinSchemeOption{PinyinScheme::Full, "full", "全拼 / 简拼"},
    PinyinSchemeOption{PinyinScheme::Xiaohe, "xiaohe", "小鹤双拼"},
    PinyinSchemeOption{PinyinScheme::Ziranma, "ziranma", "自然码双拼"},
};

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
