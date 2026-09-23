#include "modernime/core/pinyin_match.h"
#include "modernime/core/english_dictionary.h"

#include <array>
#include <cctype>
#include <vector>

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

bool PinyinMatchPolicy::initialsMatch(std::string_view userInput,
                                      std::string_view fullPinyin) {
    if (userInput.empty() || fullPinyin.empty()) {
        return false;
    }
    std::vector<std::string_view> syllables;
    std::size_t start = 0;
    while (start < fullPinyin.size()) {
        auto pos = fullPinyin.find('\'', start);
        if (pos == std::string_view::npos) {
            pos = fullPinyin.size();
        }
        if (pos > start) {
            syllables.push_back(fullPinyin.substr(start, pos - start));
        }
        start = pos + 1;
    }
    if (syllables.empty()) {
        return false;
    }
    auto matchNext = [&](auto self, std::size_t uIdx, std::size_t sIdx) -> bool {
        if (uIdx == userInput.size() && sIdx == syllables.size()) {
            return true;
        }
        if (uIdx >= userInput.size() || sIdx >= syllables.size()) {
            return false;
        }
        const auto &syl = syllables[sIdx];
        if (syl.empty()) {
            return false;
        }
        if (syl.size() >= 2 && (syl.starts_with("zh") || syl.starts_with("ch") || syl.starts_with("sh"))) {
            if (userInput.substr(uIdx).starts_with(syl.substr(0, 2))) {
                if (self(self, uIdx + 2, sIdx + 1)) {
                    return true;
                }
            }
        }
        if (userInput[uIdx] == syl[0]) {
            if (self(self, uIdx + 1, sIdx + 1)) {
                return true;
            }
        }
        return false;
    };
    return matchNext(matchNext, 0, 0);
}

bool PinyinMatchPolicy::exactInputMatch(std::string_view userInput,
                                        std::string_view fullPinyin) {
    return canonical(fullPinyin) == canonical(userInput);
}

bool PinyinMatchPolicy::trustedShortAbbreviationMatch(
    std::string_view userInput, std::string_view fullPinyin,
    std::string_view text) {
    if (!isAbbreviationInput(userInput)) {
        return false;
    }
    if (abbreviationKey(fullPinyin) != userInput &&
        !initialsMatch(userInput, fullPinyin)) {
        return false;
    }

    struct TrustedEntry {
        std::string_view input;
        std::string_view phrase;
    };
    static constexpr std::array commonPhrases{
        // 2-letter
        TrustedEntry{"bj", "北京"},
        TrustedEntry{"dl", "登录"},
        TrustedEntry{"sh", "上海"},
        TrustedEntry{"gz", "广州"},
        TrustedEntry{"sz", "深圳"},
        TrustedEntry{"sz", "设置"},
        TrustedEntry{"hz", "杭州"},
        TrustedEntry{"cd", "成都"},
        TrustedEntry{"wh", "武汉"},
        TrustedEntry{"cs", "长沙"},
        TrustedEntry{"cq", "重庆"},
        TrustedEntry{"nj", "南京"},
        TrustedEntry{"tj", "天津"},
        TrustedEntry{"zg", "中国"},
        TrustedEntry{"rm", "人民"},
        TrustedEntry{"dx", "大学"},
        TrustedEntry{"xx", "谢谢"},
        TrustedEntry{"hy", "欢迎"},
        TrustedEntry{"hf", "回复"},
        TrustedEntry{"yd", "移动"},
        TrustedEntry{"wx", "微信"},
        TrustedEntry{"zh", "账号"},
        TrustedEntry{"mm", "密码"},
        TrustedEntry{"yx", "邮箱"},
        TrustedEntry{"sj", "手机"},
        TrustedEntry{"sj", "时间"},
        TrustedEntry{"wz", "位置"},
        TrustedEntry{"wz", "网站"},
        TrustedEntry{"lj", "链接"},
        TrustedEntry{"wj", "文件"},
        TrustedEntry{"cz", "操作"},
        TrustedEntry{"qd", "确定"},
        TrustedEntry{"qx", "取消"},
        TrustedEntry{"fh", "返回"},
        TrustedEntry{"cx", "查询"},
        TrustedEntry{"tj", "添加"},
        TrustedEntry{"sc", "删除"},
        TrustedEntry{"xz", "下载"},
        TrustedEntry{"fx", "分享"},
        TrustedEntry{"fs", "发送"},
        TrustedEntry{"js", "接收"},
        TrustedEntry{"ts", "提示"},
        TrustedEntry{"cw", "错误"},
        TrustedEntry{"cg", "成功"},
        TrustedEntry{"sb", "失败"},
        TrustedEntry{"zy", "注意"},
        TrustedEntry{"gs", "公司"},
        TrustedEntry{"xm", "项目"},
        TrustedEntry{"rw", "任务"},
        TrustedEntry{"bg", "报告"},
        TrustedEntry{"jh", "计划"},
        TrustedEntry{"hy", "会议"},
        TrustedEntry{"sq", "申请"},
        TrustedEntry{"tz", "通知"},
        // 3-letter
        TrustedEntry{"wsm", "为什么"},
        TrustedEntry{"msm", "没什么"},
        TrustedEntry{"zmy", "怎么样"},
        TrustedEntry{"zmb", "怎么办"},
        TrustedEntry{"bzd", "不知道"},
        TrustedEntry{"mwt", "没问题"},
        TrustedEntry{"dqb", "对不起"},
        TrustedEntry{"dbq", "对不起"},
        TrustedEntry{"tql", "太强了"},
        TrustedEntry{"gkd", "搞快点"},
        TrustedEntry{"mgx", "没关系"},
        TrustedEntry{"xxn", "谢谢你"},
        TrustedEntry{"wzd", "我知道"},
        TrustedEntry{"yqc", "一起吃"},
        TrustedEntry{"yqch", "一起吃"},
        TrustedEntry{"rmb", "人民币"},
        // 4-letter / idioms
        TrustedEntry{"yqhc", "一气呵成"},
        TrustedEntry{"yyds", "永远的神"},
        TrustedEntry{"yysy", "有一说一"},
        TrustedEntry{"xswl", "笑死我了"},
        TrustedEntry{"awsl", "啊我死了"},
        TrustedEntry{"yqh", "爷青回"},
        TrustedEntry{"pfl", "破防了"},
        TrustedEntry{"jjz", "绝绝子"},
        TrustedEntry{"ptdfg", "泼天的富贵"},
        TrustedEntry{"tkl", "泰裤辣"},
        TrustedEntry{"sckx", "双厨狂喜"},
    };
    for (const auto &entry : commonPhrases) {
        if (userInput == entry.input && text == entry.phrase) {
            return true;
        }
    }
    return false;
}

std::string PinyinMatchPolicy::normalizeTypoInput(std::string_view input) {
    if (input.empty()) {
        return "";
    }
    // 保护合法英文前缀（如 garag -> garage, appl -> apple），避免被拼音容错改写为伪拼音
    const auto englishPredictions = EnglishDictionary::predictWords(input, 1);
    if (!englishPredictions.empty() && englishPredictions.front() != input) {
        return std::string(input);
    }

    // 保护合法英文单词（尤其是含连续重元音的英文如 good, book, deep 等），避免被抗抖误去重
    const bool isKnownEnglish = EnglishDictionary::isEnglishWord(input);

    const auto isVowel = [](char c) {
        return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'v';
    };
    const auto isConsonant = [&isVowel](char c) {
        return (c >= 'a' && c <= 'z') && !isVowel(c);
    };

    std::string s(input);

    // 1. 抗抖去重：消除单音节内的非法连续双元音 (oo->o, ee->e, aa->a, ii->i, uu->u)
    if (!isKnownEnglish) {
        std::string debounced;
        debounced.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i > 0 && isVowel(s[i]) && s[i] == s[i - 1]) {
                continue;
            }
            if (i > 0 && isConsonant(s[i]) && s[i] == s[i - 1] && (i == 1 || s[i - 2] == '\'')) {
                continue;
            }
            debounced.push_back(s[i]);
        }
        s = std::move(debounced);
    }

    // 2. QWERTY 邻键误触置换 (在漏字补全前优先处理)
    // 2.1 yop -> you (e.g. yop -> you, womenyopqu -> womenyouqu)
    std::size_t pos = 0;
    while ((pos = s.find("yop", pos)) != std::string::npos) {
        const bool atEndOrConsonant = (pos + 3 == s.size() || s[pos + 3] == '\'' || isConsonant(s[pos + 3]));
        if (atEndOrConsonant) {
            s.replace(pos, 3, "you");
            pos += 3;
        } else {
            pos += 3;
        }
    }

    // 2.2 omg -> ong (e.g. zhomg -> zhong)
    pos = 0;
    while ((pos = s.find("omg", pos)) != std::string::npos) {
        s.replace(pos, 3, "ong");
        pos += 3;
    }

    // 2.3 末尾 up -> uo (e.g. bucup -> bucuo)
    if (s.size() >= 3 && s.ends_with("up") && isConsonant(s[s.size() - 3])) {
        s[s.size() - 1] = 'o';
    }

    // 2.4 xiab / jiab / qiab -> xian / jian / qian (b <-> n)
    for (const auto &pre : {"xiab", "jiab", "qiab", "liab", "guab", "kuab"}) {
        pos = 0;
        while ((pos = s.find(pre, pos)) != std::string::npos) {
            const bool atEndOrConsonant = (pos + 4 == s.size() || s[pos + 4] == '\'' || isConsonant(s[pos + 4]));
            if (atEndOrConsonant) {
                s[pos + 3] = 'n';
                pos += 4;
            } else {
                pos += 4;
            }
        }
    }

    // 3. 倒序换位修复 (Transpositions)
    // 3.1 gn -> ng, mg -> ng
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if ((s[i] == 'g' || s[i] == 'm') && s[i + 1] == 'n' && isVowel(s[i - 1])) {
            s[i] = 'n';
            s[i + 1] = 'g';
        } else if (s[i] == 'm' && s[i + 1] == 'g' && isVowel(s[i - 1])) {
            s[i] = 'n';
            s[i + 1] = 'g';
        }
    }

    // 3.2 agn -> ang, ogn -> ong, ugn -> ung, egn -> eng, ign -> ing
    for (const auto &p : {std::pair{"agn", "ang"}, std::pair{"ogn", "ong"},
                          std::pair{"ugn", "ung"}, std::pair{"egn", "eng"},
                          std::pair{"ign", "ing"}}) {
        pos = 0;
        while ((pos = s.find(p.first, pos)) != std::string::npos) {
            s.replace(pos, 3, p.second);
            pos += 3;
        }
    }

    // 3.3 fna -> fan
    pos = 0;
    while ((pos = s.find("fna", pos)) != std::string::npos) {
        s.replace(pos, 3, "fan");
        pos += 3;
    }

    // 3.4 ina -> ian (e.g. tina -> tian, xina -> xian)
    pos = 0;
    while ((pos = s.find("ina", pos)) != std::string::npos) {
        if (pos > 0 && isConsonant(s[pos - 1])) {
            s.replace(pos, 3, "ian");
            pos += 3;
        } else {
            ++pos;
        }
    }

    // 3.5 una -> uan (e.g. guna -> guan, kuna -> kuan, tuna -> tuan)
    pos = 0;
    while ((pos = s.find("una", pos)) != std::string::npos) {
        if (pos > 0 && isConsonant(s[pos - 1])) {
            s.replace(pos, 3, "uan");
            pos += 3;
        } else {
            ++pos;
        }
    }

    // 3.6 三元音简化 uei -> ui, iou -> iu, uen -> un
    pos = 0;
    while ((pos = s.find("uei", pos)) != std::string::npos) {
        if (pos > 0 && !isVowel(s[pos - 1])) {
            s.erase(pos + 1, 1);
        } else {
            ++pos;
        }
    }
    pos = 0;
    while ((pos = s.find("iou", pos)) != std::string::npos) {
        if (pos > 0 && !isVowel(s[pos - 1])) {
            s.erase(pos + 1, 1);
        } else {
            ++pos;
        }
    }
    pos = 0;
    while ((pos = s.find("uen", pos)) != std::string::npos) {
        if (pos > 0 && !isVowel(s[pos - 1])) {
            s.erase(pos + 1, 1);
        } else {
            ++pos;
        }
    }
    // 3.7 lue -> lve, nue -> nve (LibIME 内部采用 v 代替 ü)
    pos = 0;
    while ((pos = s.find("ue", pos)) != std::string::npos) {
        if (pos > 0 && (s[pos - 1] == 'l' || s[pos - 1] == 'n')) {
            s[pos] = 'v';
        }
        pos += 2;
    }

    // 4. 残缺韵尾漏字母补全 (Omission Recovery)
    // 4.1 漏打后鼻音 n: *og -> *ong, *eg -> *eng, *ig -> *ing, *ag -> *ang
    std::string omissionRes;
    omissionRes.reserve(s.size() + 4);
    for (std::size_t i = 0; i < s.size(); ++i) {
        omissionRes.push_back(s[i]);
        if (s[i] == 'g') {
            if (i >= 1 && isVowel(s[i - 1]) && (i < 2 || s[i - 2] != 'n')) {
                const bool atEnd = (i + 1 == s.size());
                const bool nextIsConsonant = (i + 1 < s.size() && isConsonant(s[i + 1]));
                const bool nextIsSep = (i + 1 < s.size() && s[i + 1] == '\'');
                if (atEnd || nextIsConsonant || nextIsSep) {
                    omissionRes.pop_back();
                    omissionRes.push_back('n');
                    omissionRes.push_back('g');
                }
            }
        }
    }
    s = std::move(omissionRes);

    // 4.2 漏打三元音 u: *yo -> *you (例如 pengyo -> pengyou)
    pos = 0;
    while ((pos = s.find("yo", pos)) != std::string::npos) {
        const bool afterConsonant = (pos > 0 && isConsonant(s[pos - 1]));
        const bool atEndOrSep = (pos + 2 == s.size() || s[pos + 2] == '\'' || isConsonant(s[pos + 2]));
        if (afterConsonant && atEndOrSep) {
            s.insert(pos + 2, "u");
            pos += 3;
        } else {
            pos += 2;
        }
    }

    return s;
}

std::size_t PinyinMatchPolicy::matchTypoSyllable(std::string_view input,
                                                std::string_view syllable) {
    if (input.empty() || syllable.empty()) {
        return 0;
    }
    // 1. gn <-> ng (e.g. input "dign" vs syllable "ding")
    //    mg -> ng (adjacent key slip: input "dimg" vs syllable "ding")
    if (syllable.ends_with("ng")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen)) {
            const auto tail = input.substr(prefixLen, 2);
            if (tail == "gn" || tail == "mg") {
                return syllable.size();
            }
        }
    }
    // 2. ina <-> ian (e.g. input "tina" vs syllable "tian")
    if (syllable.ends_with("ian")) {
        const auto prefixLen = syllable.size() - 3;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "ina") {
            return syllable.size();
        }
    }
    // 3. una <-> uan (e.g. input "guna" vs syllable "guan")
    if (syllable.ends_with("uan")) {
        const auto prefixLen = syllable.size() - 3;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "una") {
            return syllable.size();
        }
    }
    // 4. uei -> ui (e.g. input "shuei" vs syllable "shui")
    if (syllable.ends_with("ui")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() + 1 &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "uei") {
            return syllable.size() + 1;
        }
    }
    // 5. iou -> iu (e.g. input "jiou" vs syllable "jiu")
    if (syllable.ends_with("iu")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() + 1 &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "iou") {
            return syllable.size() + 1;
        }
    }
    // 6. uen -> un (e.g. input "luen" vs syllable "lun")
    if (syllable.ends_with("un")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() + 1 &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "uen") {
            return syllable.size() + 1;
        }
    }
    // 7. ve -> ue (e.g. input "lve" vs syllable "lue", or input "lue" vs syllable "lve")
    if (syllable.ends_with("ve")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            (input.substr(prefixLen, 2) == "ue" || input.substr(prefixLen, 2) == "ve")) {
            return syllable.size();
        }
    }
    if (syllable.ends_with("ue")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 2) == "ve") {
            return syllable.size();
        }
    }
    // 8. 漏打 n 的后鼻音残缺: *og->ong, *eg->eng, *ig->ing, *ag->ang
    for (const auto &suffix : {"ong", "eng", "ing", "ang"}) {
        if (syllable.ends_with(suffix)) {
            const auto prefixLen = syllable.size() - 3;
            if (input.size() >= syllable.size() - 1 &&
                input.substr(0, prefixLen) == syllable.substr(0, prefixLen)) {
                const std::string shortSuffix{suffix[0], suffix[2]};
                if (input.substr(prefixLen, 2) == shortSuffix) {
                    return syllable.size() - 1;
                }
            }
        }
    }
    // 9. 邻键误触: omg -> ong, op -> ou, ab -> an, up -> uo (优先于漏字判定)
    if (syllable.ends_with("ong")) {
        const auto prefixLen = syllable.size() - 3;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 3) == "omg") {
            return syllable.size();
        }
    }
    if (syllable.ends_with("ou")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 2) == "op") {
            return syllable.size();
        }
    }
    if (syllable.ends_with("an")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 2) == "ab") {
            return syllable.size();
        }
    }
    if (syllable.ends_with("uo")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 2) == "up") {
            return syllable.size();
        }
    }
    // 10. 漏打 u 的三元音残缺: yo -> you (e.g. input "yo" vs syllable "you")
    if (syllable.ends_with("ou")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() - 1 &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input[prefixLen] == 'o') {
            return syllable.size() - 1;
        }
    }
    // 11. 音节内换位: na -> an (e.g. chifna -> chifan)
    if (syllable.ends_with("an")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.size() >= syllable.size() &&
            input.substr(0, prefixLen) == syllable.substr(0, prefixLen) &&
            input.substr(prefixLen, 2) == "na") {
            return syllable.size();
        }
    }
    // agn -> ang, ogn -> ong
    for (const auto &suffix : {"ang", "ong"}) {
        if (syllable.ends_with(suffix)) {
            const auto prefixLen = syllable.size() - 3;
            if (input.size() >= syllable.size() &&
                input.substr(0, prefixLen) == syllable.substr(0, prefixLen)) {
                const std::string swappedSuffix{suffix[0], suffix[2], suffix[1]};
                if (input.substr(prefixLen, 3) == swappedSuffix) {
                    return syllable.size();
                }
            }
        }
    }
    // 12. 抗抖双元音连击: e.g. oo in zhoong vs o in zhong, ee in sheeng vs e in sheng
    if (syllable.size() >= 2) {
        for (std::size_t i = 1; i < syllable.size(); ++i) {
            char v = syllable[i];
            if (v == 'a' || v == 'e' || v == 'i' || v == 'o' || v == 'u') {
                if (input.size() >= syllable.size() + 1 &&
                    input.substr(0, i) == syllable.substr(0, i) &&
                    input[i] == v && input[i + 1] == v &&
                    input.substr(i + 2, syllable.size() - (i + 1)) == syllable.substr(i + 1)) {
                    return syllable.size() + 1;
                }
            }
        }
    }
    return 0;
}

bool PinyinMatchPolicy::isTypoPrefix(std::string_view input,
                                     std::string_view syllable) {
    if (input.empty() || syllable.empty()) {
        return false;
    }
    if (syllable.ends_with("ng")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "g" || tail == "m" || tail == "gn" || tail == "mg") {
                return true;
            }
        }
    }
    for (const auto &suffix : {"ong", "eng", "ing", "ang"}) {
        if (syllable.ends_with(suffix)) {
            const auto prefixLen = syllable.size() - 3;
            if (input.starts_with(syllable.substr(0, prefixLen))) {
                const auto tail = input.substr(prefixLen);
                if (tail.size() >= 1 && tail.front() == suffix[0]) {
                    return true;
                }
            }
        }
    }
    if (syllable.ends_with("ui")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "u" || tail == "ue") {
                return true;
            }
        }
    }
    if (syllable.ends_with("iu")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "i" || tail == "io") {
                return true;
            }
        }
    }
    if (syllable.ends_with("un")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "u" || tail == "ue") {
                return true;
            }
        }
    }
    if (syllable.ends_with("ian")) {
        const auto prefixLen = syllable.size() - 3;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "i" || tail == "in" || tail == "ina") {
                return true;
            }
        }
    }
    if (syllable.ends_with("uan")) {
        const auto prefixLen = syllable.size() - 3;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "u" || tail == "un" || tail == "una") {
                return true;
            }
        }
    }
    if (syllable.ends_with("ou")) {
        const auto prefixLen = syllable.size() - 2;
        if (input.starts_with(syllable.substr(0, prefixLen))) {
            const auto tail = input.substr(prefixLen);
            if (tail == "y" || tail == "yo" || tail == "o" || tail == "op") {
                return true;
            }
        }
    }
    return false;
}

bool PinyinMatchPolicy::isFullTypoMatch(std::string_view userInput,
                                        std::string_view fullPinyin) {
    if (userInput.empty() || fullPinyin.empty()) {
        return false;
    }
    const auto preds = EnglishDictionary::predictWords(userInput, 1);
    if (!preds.empty() && preds.front() != userInput &&
        !exactInputMatch(userInput, fullPinyin)) {
        return false;
    }
    std::vector<std::string_view> syllables;
    std::size_t start = 0;
    while (start < fullPinyin.size()) {
        auto pos = fullPinyin.find('\'', start);
        if (pos == std::string_view::npos) {
            pos = fullPinyin.size();
        }
        if (pos > start) {
            syllables.push_back(fullPinyin.substr(start, pos - start));
        }
        start = pos + 1;
    }
    if (syllables.empty()) {
        return false;
    }

    std::size_t offset = 0;
    bool hadTypo = false;
    for (const auto syl : syllables) {
        if (offset >= userInput.size()) {
            return false;
        }
        const auto rem = userInput.substr(offset);
        if (rem.starts_with(syl)) {
            offset += syl.size();
            continue;
        }
        const auto typoLen = matchTypoSyllable(rem, syl);
        if (typoLen > 0) {
            offset += typoLen;
            hadTypo = true;
            continue;
        }
        return false;
    }
    return offset == userInput.size() && hadTypo;
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
    if (isFullTypoMatch(userInput, fullPinyin)) {
        return 2;
    }
    // isAbbreviationInput 只依赖 userInput，对所有候选结果相同，
    // 先短路再做依赖候选的 abbreviationKey。
    if (isAbbreviationInput(userInput) &&
        (abbreviationKey(fullPinyin) == userInput ||
         initialsMatch(userInput, fullPinyin))) {
        return 1;
    }
    if (!candidate.empty() && candidate.size() < input.size() &&
        input.substr(0, candidate.size()) == candidate) {
        return -1;
    }
    return 0;
}

} // namespace modernime::core
