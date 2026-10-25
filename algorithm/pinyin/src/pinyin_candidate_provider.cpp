#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"

#include "modernime/pinyin/candidate_composition.h"
#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/candidate_prefixes.h"
#include "modernime/pinyin/recent_choice.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/reading_hints.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/english_definition_dictionary.h"
#include "modernime/core/learning_writer.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/phrase_formation.h"
#include "modernime/core/settings.h"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>

#include <algorithm>
#include <ctime>
#include <unordered_map>

namespace {

inline libime::PinyinFuzzyFlags getDefaultFuzzyFlags() {
    libime::PinyinFuzzyFlags flags{
        libime::PinyinFuzzyFlag::Z_ZH,
        libime::PinyinFuzzyFlag::C_CH,
        libime::PinyinFuzzyFlag::S_SH,
        libime::PinyinFuzzyFlag::L_N,
        libime::PinyinFuzzyFlag::EN_ENG,
        libime::PinyinFuzzyFlag::IN_ING,
    };
#ifdef MODERNIME_HAS_PINYIN_COMMON_TYPO
    flags |= libime::PinyinFuzzyFlag::CommonTypo;
#endif
    return flags;
}

std::string toChineseFinancial(std::string_view numStr) {
    if (numStr.empty()) {
        return {};
    }
    auto dotPos = numStr.find('.');
    auto intPart = numStr.substr(0, dotPos);
    std::string decPart = (dotPos != std::string_view::npos) ? std::string(numStr.substr(dotPos + 1)) : "";

    auto firstNonZero = intPart.find_first_not_of('0');
    if (firstNonZero != std::string_view::npos) {
        intPart = intPart.substr(firstNonZero);
    } else {
        intPart = "";
    }

    static const char *const kDigits[] = {
        "零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"
    };
    static const char *const kUnits[] = {"", "拾", "佰", "仟"};
    static const char *const kBigUnits[] = {"", "万", "亿", "兆"};

    if (intPart.empty() && decPart.empty()) {
        return "零元整";
    }

    std::string result;
    if (intPart.empty()) {
        result = "零元";
    } else {
        std::vector<std::string> groups;
        std::string s(intPart);
        while (!s.empty()) {
            if (s.size() > 4) {
                groups.push_back(s.substr(s.size() - 4));
                s.resize(s.size() - 4);
            } else {
                groups.push_back(s);
                s.clear();
            }
        }
        std::vector<std::string> groupResults;
        for (std::size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
            const auto &grp = groups[gIdx];
            std::size_t gLen = grp.size();
            std::string gText;
            bool zeroFlag = false;
            for (std::size_t i = 0; i < gLen; ++i) {
                int d = grp[i] - '0';
                if (d < 0 || d > 9) continue;
                std::size_t unitIdx = gLen - 1 - i;
                if (d == 0) {
                    zeroFlag = true;
                } else {
                    if (zeroFlag) {
                        gText += "零";
                        zeroFlag = false;
                    }
                    gText += kDigits[d];
                    if (unitIdx < 4) {
                        gText += kUnits[unitIdx];
                    }
                }
            }
            if (!gText.empty()) {
                if (gIdx < 4) {
                    gText += kBigUnits[gIdx];
                }
                groupResults.push_back(gText);
            }
        }
        std::string intStr;
        for (auto it = groupResults.rbegin(); it != groupResults.rend(); ++it) {
            intStr += *it;
        }
        result = intStr + "元";
    }

    if (decPart.empty() || decPart == "0" || decPart == "00") {
        result += "整";
    } else {
        int jiao = (decPart.size() > 0 && decPart[0] >= '0' && decPart[0] <= '9') ? (decPart[0] - '0') : 0;
        int fen = (decPart.size() > 1 && decPart[1] >= '0' && decPart[1] <= '9') ? (decPart[1] - '0') : 0;
        if (jiao > 0) {
            result += kDigits[jiao];
            result += "角";
        } else if (fen > 0) {
            result += "零";
        }
        if (fen > 0) {
            result += kDigits[fen];
            result += "分";
        } else if (jiao > 0) {
            result += "整";
        }
    }
    return result;
}

std::string toChineseStandardNumber(std::string_view numStr) {
    if (numStr.empty()) {
        return {};
    }
    auto dotPos = numStr.find('.');
    auto intPart = numStr.substr(0, dotPos);
    std::string decPart = (dotPos != std::string_view::npos) ? std::string(numStr.substr(dotPos + 1)) : "";

    auto firstNonZero = intPart.find_first_not_of('0');
    if (firstNonZero != std::string_view::npos) {
        intPart = intPart.substr(firstNonZero);
    } else {
        intPart = "";
    }

    static const char *const kDigits[] = {
        "零", "一", "二", "三", "四", "五", "六", "七", "八", "九"
    };
    static const char *const kUnits[] = {"", "十", "百", "千"};
    static const char *const kBigUnits[] = {"", "万", "亿", "兆"};

    if (intPart.empty() && decPart.empty()) {
        return "零";
    }

    std::string result;
    if (intPart.empty()) {
        result = "零";
    } else {
        std::vector<std::string> groups;
        std::string s(intPart);
        while (!s.empty()) {
            if (s.size() > 4) {
                groups.push_back(s.substr(s.size() - 4));
                s.resize(s.size() - 4);
            } else {
                groups.push_back(s);
                s.clear();
            }
        }
        std::vector<std::string> groupResults;
        for (std::size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
            const auto &grp = groups[gIdx];
            std::size_t gLen = grp.size();
            std::string gText;
            bool zeroFlag = false;
            for (std::size_t i = 0; i < gLen; ++i) {
                int d = grp[i] - '0';
                if (d < 0 || d > 9) continue;
                std::size_t unitIdx = gLen - 1 - i;
                if (d == 0) {
                    zeroFlag = true;
                } else {
                    if (zeroFlag) {
                        gText += "零";
                        zeroFlag = false;
                    }
                    if (d == 1 && unitIdx == 1 && gLen == 2 && gIdx == groups.size() - 1 && gText.empty()) {
                        gText += kUnits[unitIdx];
                    } else {
                        gText += kDigits[d];
                        if (unitIdx < 4) {
                            gText += kUnits[unitIdx];
                        }
                    }
                }
            }
            if (!gText.empty()) {
                if (gIdx < 4) {
                    gText += kBigUnits[gIdx];
                }
                groupResults.push_back(gText);
            }
        }
        for (auto it = groupResults.rbegin(); it != groupResults.rend(); ++it) {
            result += *it;
        }
    }
    if (!decPart.empty()) {
        result += "点";
        for (char c : decPart) {
            if (c >= '0' && c <= '9') {
                result += kDigits[c - '0'];
            }
        }
    }
    return result;
}

std::vector<std::string> generateMacroCandidates(std::string_view rawInput,
                                                 std::string_view canonicalInput = {}) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::vector<std::string> results;

    // v-mode financial & numeral converter
    if (rawInput.size() > 1 && rawInput.front() == 'v') {
        std::string_view numPart = rawInput.substr(1);
        bool validNum = true;
        bool hasDot = false;
        for (char c : numPart) {
            if (c >= '0' && c <= '9') continue;
            if (c == '.' && !hasDot) { hasDot = true; continue; }
            validNum = false;
            break;
        }
        if (validNum) {
            const auto fin = toChineseFinancial(numPart);
            if (!fin.empty()) results.push_back(fin);
            const auto stdNum = toChineseStandardNumber(numPart);
            if (!stdNum.empty()) results.push_back(stdNum);
            results.emplace_back(numPart);
            return results;
        }
    }

    std::string_view lookupKey = canonicalInput.empty() ? rawInput : canonicalInput;

    if (lookupKey == "rq" || rawInput == "rq") {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d年%d月%d日", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        results.push_back(buf);
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        results.push_back(buf);
    } else if (lookupKey == "sj" || rawInput == "sj") {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        results.push_back(buf);
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        results.push_back(buf);
    } else if (lookupKey == "xq" || rawInput == "xq") {
        static const char *const kWeekdays[] = {
            "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
        };
        static const char *const kZhou[] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六"
        };
        if (tm.tm_wday >= 0 && tm.tm_wday <= 6) {
            results.push_back(kWeekdays[tm.tm_wday]);
            results.push_back(kZhou[tm.tm_wday]);
        }
    }

    static const std::unordered_map<std::string_view, std::vector<const char *>> kMacroMap = {
        // Chinese punctuation marks & symbols
        {"gantanhao", {"！", "!", "❗️"}},
        {"tanhao", {"！", "!", "❗️"}},
        {"jingtanhao", {"！", "!", "❗️"}},
        {"wenhao", {"？", "?", "❓"}},
        {"douhao", {"，", ","}},
        {"juhao", {"。", "."}},
        {"dunhao", {"、"}},
        {"fenhao", {"；", ";"}},
        {"maohao", {"：", ":"}},
        {"shenglvehao", {"……", "…"}},
        {"shengvehao", {"……", "…"}},
        {"pozhehao", {"——", "—"}},
        {"pozehao", {"——", "—"}},
        {"bolangxian", {"～", "~"}},
        {"bolanghao", {"～", "~"}},
        {"bozhehao", {"～", "~"}},
        {"boxian", {"～", "~"}},
        {"shuminghao", {"《》", "《", "》"}},
        {"zuoshuminghao", {"《", "〈"}},
        {"youshuminghao", {"》", "〉"}},
        {"yinhao", {"“”", "‘’", "“", "”"}},
        {"shuangyinhao", {"“”", "“", "”"}},
        {"danyinhao", {"‘’", "‘", "’"}},
        {"zuoyinhao", {"“", "‘"}},
        {"youyinhao", {"”", "’"}},
        {"kuohao", {"（）", "()", "（", "）"}},
        {"yuankuohao", {"（）", "()"}},
        {"xiaokuohao", {"（）", "()"}},
        {"fangkuohao", {"【】", "[]"}},
        {"zhongkuohao", {"【】", "[]"}},
        {"dakuohao", {"｛｝", "{}"}},
        {"huakuohao", {"｛｝", "{}"}},
        {"zuokuohao", {"（", "(", "【", "["}},
        {"youkuohao", {"）", ")", "】", "]"}},
        {"jiangehao", {"·", "•"}},
        {"xiahuaxian", {"_"}},
        {"lianjiehao", {"-", "－"}},
        {"xiegang", {"/"}},
        {"zhengxiegang", {"/"}},
        {"fanxiegang", {"\\"}},

        // Greek letters
        {"pi", {"π", "Π"}},
        {"alpha", {"α", "Α"}},
        {"beta", {"β", "Β"}},
        {"gamma", {"γ", "Γ"}},
        {"delta", {"δ", "Δ"}},
        {"theta", {"θ", "Θ"}},
        {"lambda", {"λ", "Λ"}},
        {"mu", {"μ"}},
        {"sigma", {"σ", "Σ"}},
        {"omega", {"ω", "Ω"}},
        {"epsilon", {"ε"}},
        {"phi", {"φ", "Φ"}},
        {"tau", {"τ"}},
        {"rho", {"ρ"}},

        // Math & physics & units
        {"jiahao", {"+", "＋"}},
        {"jianhao", {"-", "－"}},
        {"chenghao", {"×", "*"}},
        {"chuhao", {"÷", "/"}},
        {"denghao", {"=", "＝"}},
        {"dengyu", {"=", "＝"}},
        {"budeng", {"≠"}},
        {"budenghao", {"≠"}},
        {"yuedeng", {"≈"}},
        {"yuedenghao", {"≈"}},
        {"dayu", {">", "＞"}},
        {"dayuhao", {">", "＞"}},
        {"xiaoyu", {"<", "＜"}},
        {"xiaoyuhao", {"<", "＜"}},
        {"dayudeng", {"≥"}},
        {"dayudengyu", {"≥", ">="}},
        {"dayudengyuhao", {"≥", ">="}},
        {"xiaoyudeng", {"≤"}},
        {"xiaoyudengyu", {"≤", "<="}},
        {"xiaoyudengyuhao", {"≤", "<="}},
        {"zhengfu", {"±"}},
        {"zhengfuhao", {"±"}},
        {"kaifang", {"√"}},
        {"genghao", {"√"}},
        {"baifenhao", {"%", "％"}},
        {"baifenbi", {"%", "％"}},
        {"qianfenhao", {"‰"}},
        {"qianfenbi", {"‰"}},
        {"wuxian", {"∞"}},
        {"wuxianhao", {"∞"}},
        {"wuqiong", {"∞"}},
        {"wuqionghao", {"∞"}},
        {"sheshidu", {"℃"}},
        {"huashidu", {"℉"}},
        {"du", {"°"}},
        {"jiaodu", {"°"}},
        {"pingfang", {"²"}},
        {"lifang", {"³"}},
        {"suoyi", {"∴"}},
        {"yuanyin", {"∵"}},
        {"yinwei", {"∵"}},

        // Shapes & arrows
        {"dui", {"√", "✔"}},
        {"duihao", {"√", "✔"}},
        {"gou", {"√", "✔"}},
        {"cuo", {"×", "✖"}},
        {"cuohao", {"×", "✖"}},
        {"cha", {"×", "✖"}},
        {"jiantou", {"→", "←", "↑", "↓"}},
        {"xiangyou", {"→"}},
        {"youjiantou", {"→"}},
        {"xiangzuo", {"←"}},
        {"zuojiantou", {"←"}},
        {"xiangshang", {"↑"}},
        {"shangjiantou", {"↑"}},
        {"xiangxia", {"↓"}},
        {"xiajiantou", {"↓"}},
        {"you", {"→"}},
        {"zuo", {"←"}},
        {"shang", {"↑"}},
        {"xia", {"↓"}},
        {"sanjiao", {"▲", "△"}},
        {"sanjiaoxing", {"▲", "△"}},
        {"wujiaoxing", {"★", "☆"}},
        {"xingxing", {"★", "☆"}},
        {"lingxing", {"◆", "◇"}},
        {"yuan", {"●", "○"}},

        // Currencies & stats & symbols
        {"rmb", {"¥", "￥"}},
        {"renminbi", {"¥", "￥"}},
        {"dollar", {"$", "＄"}},
        {"meiyuan", {"$", "＄"}},
        {"ouyuan", {"€"}},
        {"bang", {"£"}},
        {"yingbang", {"£"}},
        {"riyuan", {"¥", "円"}},
        {"jinghao", {"#"}},
        {"aite", {"@"}},
        {"at", {"@"}},
        {"yuhao", {"&"}},
        {"and", {"&"}},

        // Legal & Ordinals
        {"jiefu", {"§"}},
        {"duanluo", {"¶"}},
        {"banquan", {"©"}},
        {"shangbiao", {"®", "™"}},
        {"xuhao", {"①", "②", "③", "④", "⑤", "⑥", "⑦", "⑧", "⑨", "⑩"}},
        {"yuanquan", {"①", "②", "③", "④", "⑤", "○", "●"}},

        // Emojis & Chat
        {"haha", {"😄", "😂"}},
        {"xixi", {"😆"}},
        {"heihei", {"😁"}},
        {"zan", {"👍"}},
        {"dianzan", {"👍"}},
        {"xin", {"❤️"}},
        {"aixin", {"❤️"}},
        {"ku", {"😭"}},
        {"ok", {"👌"}},
        {"bixin", {"🫰"}},
        {"bai", {"👋"}},
        {"zaijian", {"👋"}},
        {"baoquan", {"🙏"}},
        {"fennu", {"😡"}},
        {"niu", {"🐂"}},
        {"daniu", {"🐂"}},
        {"niubi", {"🐂"}},
        {"liuliuliu", {"🐂"}},
        {"doge", {"🐶"}},
        {"tangping", {"🛌"}},
        {"jiayou", {"💪"}},
    };

    auto it = kMacroMap.find(lookupKey);
    if (it == kMacroMap.end() && !rawInput.empty() && rawInput != lookupKey) {
        it = kMacroMap.find(rawInput);
    }
    if (it != kMacroMap.end()) {
        for (const auto *val : it->second) {
            results.emplace_back(val);
        }
    }

    return results;
}

} // namespace
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modernime::pinyin {

namespace {

std::int64_t nowMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::size_t pinyinLetterCount(std::string_view input) {
    std::size_t count = 0;
    for (const char character : input) {
        if (character >= 'a' && character <= 'z') {
            ++count;
        }
    }
    return count;
}

std::string candidateKey(std::string_view pinyin, std::string_view text) {
    std::string key = core::normalizePinyin(pinyin);
    key.push_back('\x1f');
    key.append(text);
    return key;
}

std::vector<std::size_t> syllablePrefixEnds(std::string_view rawInput,
                                            std::string_view segmentedInput) {
    std::vector<std::size_t> result;
    std::size_t rawOffset = 0;
    for (const char character : segmentedInput) {
        if (character != '\'') {
            while (rawOffset < rawInput.size() && rawInput[rawOffset] == '\'') {
                ++rawOffset;
            }
            if (rawOffset < rawInput.size()) {
                ++rawOffset;
            }
            continue;
        }
        if (rawOffset < rawInput.size() && rawInput[rawOffset] == '\'') {
            ++rawOffset;
        }
        if (rawOffset > 0 && rawOffset < rawInput.size()) {
            result.push_back(rawOffset);
        }
    }
    return result;
}

std::string prefixInput(std::string_view rawInput, std::size_t consumedBytes) {
    auto prefix = std::string(rawInput.substr(0, consumedBytes));
    while (!prefix.empty() && prefix.back() == '\'') {
        prefix.pop_back();
    }
    return prefix;
}

// Check if input substring matches a standard pinyin syllable with typo tolerance.
// Returns the number of characters consumed from input; 0 if no match.
std::size_t matchTypoSyllable(std::string_view input, std::string_view syllable) {
    return core::PinyinMatchPolicy::matchTypoSyllable(input, syllable);
}

bool isTypoPrefix(std::string_view input, std::string_view syllable) {
    return core::PinyinMatchPolicy::isTypoPrefix(input, syllable);
}

std::string correctTypoInput(std::string_view input) {
    return core::PinyinMatchPolicy::normalizeTypoInput(input);
}

bool coversPinyinInput(std::string_view userInput,
                       const std::string &canonicalInput,
                       std::string_view fullPinyin) {
    if (core::PinyinMatchPolicy::isFullTypoMatch(userInput, fullPinyin)) {
        return true;
    }

    // Keep short ASCII words such as "who" as the English fallback. Chinese
    // initialisms become a reliable signal once they contain at least four
    // initials, which also covers the normal four-character idiom case.
    if (userInput.size() >= 4 &&
        core::PinyinMatchPolicy::isAbbreviationInput(userInput) &&
        (core::PinyinMatchPolicy::abbreviationKey(fullPinyin) == userInput ||
         core::PinyinMatchPolicy::initialsMatch(userInput, fullPinyin))) {
        return true;
    }

    const auto &input = canonicalInput;
    const auto candidate = core::PinyinMatchPolicy::canonical(fullPinyin);
    if (input.empty()) {
        return false;
    }
    if (candidate.size() >= input.size() &&
        candidate.compare(0, input.size(), input) == 0) {
        return true;
    }

    std::vector<std::string_view> syllables;
    std::size_t syllableStart = 0;
    while (syllableStart < fullPinyin.size()) {
        const auto separator = fullPinyin.find('\'', syllableStart);
        const auto syllableEnd = separator == std::string_view::npos
                                     ? fullPinyin.size()
                                     : separator;
        if (syllableEnd == syllableStart) {
            return false;
        }
        syllables.push_back(
            fullPinyin.substr(syllableStart, syllableEnd - syllableStart));
        if (separator == std::string_view::npos) {
            break;
        }
        syllableStart = separator + 1;
    }

    std::size_t inputOffset = 0;
    bool abbreviationMode = false;
    bool consumedFullSyllable = false;
    for (const auto syllable : syllables) {
        if (inputOffset == input.size()) {
            return consumedFullSyllable;
        }
        const auto remaining = input.substr(inputOffset);
        if (!abbreviationMode && remaining.starts_with(syllable)) {
            inputOffset += syllable.size();
            consumedFullSyllable = true;
            continue;
        }
        if (!abbreviationMode) {
            const auto typoLen = matchTypoSyllable(remaining, syllable);
            if (typoLen > 0) {
                inputOffset += typoLen;
                consumedFullSyllable = true;
                continue;
            }
        }
        if (remaining.size() <= syllable.size() &&
            syllable.compare(0, remaining.size(), remaining) == 0) {
            return consumedFullSyllable;
        }
        if (isTypoPrefix(remaining, syllable)) {
            return consumedFullSyllable;
        }
        if (remaining.front() == syllable.front()) {
            abbreviationMode = true;
            ++inputOffset;
            continue;
        }
        return false;
    }
    return inputOffset == input.size() && consumedFullSyllable;
}

struct PreeditAlignment final {
    std::string text;
    std::size_t fullSyllableCount = 0;
    std::size_t abbreviationSyllableCount = 0;
};

bool isBetterPreeditAlignment(const PreeditAlignment &candidate,
                              const PreeditAlignment &current) {
    if (candidate.fullSyllableCount != current.fullSyllableCount) {
        return candidate.fullSyllableCount > current.fullSyllableCount;
    }
    if (candidate.abbreviationSyllableCount !=
        current.abbreviationSyllableCount) {
        return candidate.abbreviationSyllableCount >
               current.abbreviationSyllableCount;
    }
    return candidate.text.size() > current.text.size();
}

std::optional<PreeditAlignment> alignPreeditToCandidate(
    std::string_view rawInput, std::string_view fullPinyin) {
    if (rawInput.empty() || fullPinyin.empty() ||
        rawInput.find('\'') != std::string_view::npos) {
        return std::nullopt;
    }

    std::vector<std::string_view> syllables;
    std::size_t syllableStart = 0;
    while (syllableStart < fullPinyin.size()) {
        const auto separator = fullPinyin.find('\'', syllableStart);
        const auto syllableEnd = separator == std::string_view::npos
                                     ? fullPinyin.size()
                                     : separator;
        if (syllableEnd == syllableStart) {
            return std::nullopt;
        }
        syllables.push_back(
            fullPinyin.substr(syllableStart, syllableEnd - syllableStart));
        if (separator == std::string_view::npos) {
            break;
        }
        syllableStart = separator + 1;
    }
    if (syllables.empty()) {
        return std::nullopt;
    }

    const auto stateCount = (syllables.size() + 1) * (rawInput.size() + 1);
    std::vector<std::optional<PreeditAlignment>> memo(stateCount);
    std::vector<bool> visited(stateCount, false);
    const auto stateIndex = [rawInput](std::size_t syllableIndex,
                                       std::size_t inputOffset) {
        return syllableIndex * (rawInput.size() + 1) + inputOffset;
    };

    std::function<std::optional<PreeditAlignment>(std::size_t, std::size_t)>
        align = [&](std::size_t syllableIndex,
                    std::size_t inputOffset)
        -> std::optional<PreeditAlignment> {
        const auto index = stateIndex(syllableIndex, inputOffset);
        if (visited[index]) {
            return memo[index];
        }
        visited[index] = true;

        if (inputOffset == rawInput.size()) {
            memo[index] = PreeditAlignment{};
            return memo[index];
        }
        if (syllableIndex == syllables.size()) {
            return std::nullopt;
        }

        const auto syllable = syllables[syllableIndex];
        const auto remaining = rawInput.substr(inputOffset);
        std::optional<PreeditAlignment> best;
        const auto consider = [&](std::size_t consumed,
                                  bool fullSyllable,
                                  bool abbreviation) {
            const auto tail = align(syllableIndex + 1,
                                    inputOffset + consumed);
            if (!tail.has_value()) {
                return;
            }

            PreeditAlignment alignment;
            alignment.text.assign(rawInput.substr(inputOffset, consumed));
            if (!tail->text.empty()) {
                alignment.text.push_back('\'');
                alignment.text.append(tail->text);
            }
            alignment.fullSyllableCount =
                tail->fullSyllableCount + (fullSyllable ? 1 : 0);
            alignment.abbreviationSyllableCount =
                tail->abbreviationSyllableCount + (abbreviation ? 1 : 0);
            if (!best.has_value() ||
                isBetterPreeditAlignment(alignment, *best)) {
                best = std::move(alignment);
            }
        };

        if (remaining.starts_with(syllable)) {
            consider(syllable.size(), true, false);
        }

        const auto typoLen = matchTypoSyllable(remaining, syllable);
        if (typoLen > 0) {
            consider(typoLen, true, false);
        }

        if (remaining.front() == syllable.front()) {
            consider(1, false, true);
        }

        // A shorter prefix is an unfinished final syllable. It must consume
        // all remaining input; otherwise it would swallow initials belonging
        // to later syllables (for example, smcg -> s'm'c'g).
        if (remaining.size() < syllable.size() &&
            syllable.starts_with(remaining)) {
            PreeditAlignment alignment;
            alignment.text = std::string(remaining);
            if (remaining.size() == rawInput.size() - inputOffset) {
                best = !best.has_value() ||
                               isBetterPreeditAlignment(alignment, *best)
                           ? std::optional<PreeditAlignment>(
                                 std::move(alignment))
                           : best;
            }
        }

        if (isTypoPrefix(remaining, syllable) &&
            remaining.size() == rawInput.size() - inputOffset) {
            PreeditAlignment alignment;
            alignment.text = std::string(remaining);
            best = !best.has_value() ||
                           isBetterPreeditAlignment(alignment, *best)
                       ? std::optional<PreeditAlignment>(
                             std::move(alignment))
                       : best;
        }

        memo[index] = best;
        return memo[index];
    };

    auto alignment = align(0, 0);
    if (!alignment.has_value() || alignment->text.find('\'') ==
                                     std::string::npos) {
        return std::nullopt;
    }

    // A short all-initial input is intentionally left alone so ordinary
    // English fragments such as "who" are not rendered as Chinese initials.
    if (alignment->fullSyllableCount == 0 && rawInput.size() < 4) {
        return std::nullopt;
    }
    return alignment;
}

std::string automaticallySegmentedPreedit(
    std::string_view rawInput, const CandidatePipelineResult &result) {
    // An explicit separator is the user's segmentation choice.
    if (rawInput.find('\'') != std::string_view::npos) {
        return std::string(rawInput);
    }

    // Try candidates in their actual ranked order so the displayed boundary
    // follows the same phrase that the user sees first in the candidate list.
    for (const auto sourceIndex : result.order) {
        if (sourceIndex >= result.scored.size()) {
            continue;
        }
        const auto &candidate = result.scored[sourceIndex];
        const auto alignment =
            alignPreeditToCandidate(rawInput, candidate.full_pinyin);
        if (alignment.has_value()) {
            return alignment->text;
        }
    }
    return std::string(rawInput);
}

} // namespace

std::shared_ptr<PinyinCandidateProvider::SharedResources>
PinyinCandidateProvider::createSharedResources(
    const PinyinDataPaths &paths, const PinyinProviderOptions &options) {
    auto base = PinyinSharedResources::create(paths, options);
    return std::static_pointer_cast<SharedResources>(base);
}

bool PinyinCandidateProvider::reloadUserDictionary(
    std::shared_ptr<SharedResources> &resources) {
    return reloadUserDictionaryDetailed(resources).changed;
}

ResourceChange PinyinCandidateProvider::reloadUserDictionaryDetailed(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return {};
    }
    return resources->reloadUserDictionary();
}

bool PinyinCandidateProvider::reloadLearningStore(
    std::shared_ptr<SharedResources> &resources) {
    return reloadLearningStoreDetailed(resources).changed;
}

ResourceChange PinyinCandidateProvider::reloadLearningStoreDetailed(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return {};
    }
    return resources->reloadLearningStore();
}

bool PinyinCandidateProvider::refreshLearningStore(
    std::shared_ptr<SharedResources> &resources) {
    return refreshLearningStoreDetailed(resources).changed;
}

ResourceChange PinyinCandidateProvider::refreshLearningStoreDetailed(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return {};
    }
    return resources->refreshLearningStore();
}

class PinyinCandidateProvider::Impl final {
public:
    Impl(const PinyinDataPaths &paths, const PinyinProviderOptions &options)
        : Impl(createSharedResources(paths, options), options) {}

    Impl(std::shared_ptr<SharedResources> shared,
         const PinyinProviderOptions &options)
        : shared_(std::move(shared)),
          phraseTracker_(3, shared_ ? shared_->phraseStats : nullptr),
          contextLearningEnabled_(options.contextLearningEnabled),
          learningEnabled_(options.learningEnabled),
          englishDefinitionEnabled_(options.englishDefinitionEnabled),
          clipboardEnabled_(options.clipboardEnabled),
          clipboardTrigger_(options.clipboardTrigger),
          observedDictionaryGeneration_(shared_ ? shared_->dictionaryGeneration() : 0),
          observedLearningRevision_(shared_ ? shared_->learningRevision() : 0),
          observedEpochViewState_(shared_ ? shared_->epochViewState() : core::EpochViewState::Ready) {
        context = std::make_unique<libime::PinyinContext>(shared_->ime.get());
        refresh();
    }

    bool append(std::string_view input) {
        std::string nextInput = context->userInput();
        nextInput.append(input);
        if (!core::PinyinMatchPolicy::validComposition(nextInput)) {
            return false;
        }
        if (!context->type(input)) {
            return false;
        }
        refresh();
        return true;
    }

    bool eraseLast() {
        if (!context->backspace()) {
            return false;
        }
        if (context->userInput().empty()) {
            phraseTracker_.cancelComposition();
        }
        refresh();
        return true;
    }

    bool replaceInput(std::string_view input) {
        if (!input.empty() &&
            !core::PinyinMatchPolicy::validComposition(input)) {
            return false;
        }
        if (input.empty()) {
            phraseTracker_.cancelComposition();
        }
        const auto previous = context->userInput();
        context->clear();
        if (!input.empty() && !context->type(input)) {
            context->clear();
            if (!previous.empty()) {
                context->type(previous);
            }
            refresh();
            return false;
        }
        refresh();
        return true;
    }

    void onCommitEvent(const core::CommitEvent &event) {
        if (!learningEnabled_) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            return;
        }
        if (event.role == core::CommitCandidateRole::Command) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            return;
        }
        if (event.role == core::CommitCandidateRole::Raw ||
            event.role == core::CommitCandidateRole::English) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
        }
        if (auto *learning = learningWriter(); learning != nullptr) {
            learning->enqueueCommit(event);
            // An explicit positive selection restores the same canonical identity
            // in both the persistent snapshot and this context's immediate mask.
            suppressedLearned_.erase(candidateKey(event.fullPinyin, event.text));
        }
    }

    void onNegativeFeedback(std::string_view phrase, std::string_view pinyin) {
        recentChoices_.clear();
        if (auto *learning = learningWriter(); learning != nullptr) {
            learning->enqueueNegativeFeedback(phrase, pinyin);
        }
    }

    bool checkResourceChanges() {
        if (!shared_) {
            return false;
        }
        bool changed = false;
        const auto curDictGen = shared_->dictionaryGeneration();
        if (curDictGen != observedDictionaryGeneration_) {
            observedDictionaryGeneration_ = curDictGen;
            changed = true;
        }
        const auto curLearnRev = shared_->learningRevision();
        if (curLearnRev != observedLearningRevision_) {
            observedLearningRevision_ = curLearnRev;
            recentChoices_.clear();
            phraseTracker_.resetCommitHistory();
            fallbackContextBefore_.clear();
            changed = true;
        }
        const auto curViewState = shared_->epochViewState();
        if (curViewState != observedEpochViewState_) {
            observedEpochViewState_ = curViewState;
            changed = true;
        }
        if (changed) {
            revokePageAuthorization();
        }
        return changed;
    }

    void revokePageAuthorization() {
        pageAuthorized_ = false;
        ++generation;
        page_.generation = generation;
        page_.items.clear();
    }

    core::CommitEvent makeCommitEvent(
        std::string_view text,
        std::string_view fullPinyin,
        std::string_view rawKeystrokes,
        core::CommitCandidateRole role,
        std::size_t consumedInputBytes,
        bool isPartial,
        bool isDeviation,
        std::size_t candidateIndex = 0,
        std::int64_t timestampMs = 0,
        std::string_view contextBefore = {},
        std::string_view contextAfter = {}) const {
        core::CommitEvent event;
        event.text = std::string(text);
        event.fullPinyin = std::string(fullPinyin);
        event.rawKeystrokes = std::string(rawKeystrokes);
        event.role = role;
        event.consumedInputBytes = consumedInputBytes;
        event.isPartial = isPartial;
        event.isDeviation = isDeviation;
        event.candidateIndex = candidateIndex;
        event.contextBefore = std::string(contextBefore);
        event.contextAfter = std::string(contextAfter);
        event.timestampMs = timestampMs > 0 ? timestampMs : nowMilliseconds();
        return event;
    }

    std::optional<core::CandidateSelectionOutcome> selectConfirmed(
        const core::CandidateSelectionRequest &request,
        std::uint64_t providerGeneration) {
        if (checkResourceChanges()) {
            return std::nullopt;
        }
        if (!pageAuthorized_ || providerGeneration != page_.generation) {
            return std::nullopt;
        }
        if (shared_ && shared_->epochViewState() == core::EpochViewState::Reloading) {
            return std::nullopt;
        }
        if (request.globalIndex >= page_.items.size()) {
            return std::nullopt;
        }
        const auto expectedId = core::candidateIdentity(page_.items[request.globalIndex], page_);
        if (!(expectedId == request.expected)) {
            return std::nullopt;
        }

        const auto &candidate = page_.items[request.globalIndex];
        if (candidate.source == core::CandidateSource::None) {
            return std::nullopt;
        }
        core::CandidateSelectionOutcome outcome;
        outcome.selected = expectedId;

        if (candidate.source == core::CandidateSource::EnglishDefinition ||
            candidate.source == core::CandidateSource::Command) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            context->clear();
            refresh();
            outcome.action = (candidate.source == core::CandidateSource::Command)
                ? core::SelectionAction::OpenClipboard
                : core::SelectionAction::CommitText;
            outcome.remainingInput = context->userInput();
            return outcome;
        }

        const auto rawInput = context->userInput();
        const bool partialSelection = candidate.consumedInputBytes > 0 &&
                                      candidate.consumedInputBytes < rawInput.size();
        const auto consumedBytes = candidate.consumedInputBytes > 0
                                       ? candidate.consumedInputBytes
                                       : rawInput.size();
        const auto fullPinyin = candidate.fullPinyin.empty()
                                    ? (partialSelection
                                           ? std::string(rawInput.substr(0, consumedBytes))
                                           : std::string(rawInput))
                                    : candidate.fullPinyin;

        auto event = makeCommitEvent(
            candidate.text, fullPinyin, rawInput.substr(0, consumedBytes),
            expectedId.role, consumedBytes, partialSelection,
            request.globalIndex > 0, request.globalIndex, 0,
            contextBefore_, contextAfter_);

        if (!learningEnabled_) {
            onCommitEvent(event);
        } else if (partialSelection) {
            phraseTracker_.recordPartialSelection(
                candidate.text, event.fullPinyin, rawInput, event.consumedInputBytes);
            onCommitEvent(event);
        } else {
            auto formedIntra = phraseTracker_.recordFinalSelection(
                candidate.text, event.fullPinyin, rawInput, event.consumedInputBytes);
            onCommitEvent(event);
            if (formedIntra.has_value()) {
                auto phraseEvent = makeCommitEvent(
                    formedIntra->text, formedIntra->fullPinyin,
                    formedIntra->rawKeystrokes, core::CommitCandidateRole::Chinese,
                    formedIntra->rawKeystrokes.size(), false, true, 0,
                    event.timestampMs, "", "");
                onCommitEvent(phraseEvent);
                phraseTracker_.resetCommitHistory();
            } else {
                auto formedInter = phraseTracker_.recordCommit(event);
                for (const auto &fp : formedInter) {
                    auto phraseEvent = makeCommitEvent(
                        fp.text, fp.fullPinyin, fp.rawKeystrokes,
                        core::CommitCandidateRole::Chinese,
                        fp.rawKeystrokes.size(), false, true, 0,
                        event.timestampMs, "", "");
                    if (fp.isInitialFormation) {
                        for (int i = 0; i < fp.repetitionCount; ++i) {
                            onCommitEvent(phraseEvent);
                        }
                    } else {
                        onCommitEvent(phraseEvent);
                    }
                }
            }
        }

        if (candidate.source == core::CandidateSource::Raw) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            fallbackContextBefore_.clear();
            contextBefore_.clear();
            context->clear();
            refresh();
            outcome.action = core::SelectionAction::CommitText;
            outcome.remainingInput = context->userInput();
            return outcome;
        }
        if (learningEnabled_ && (event.role == core::CommitCandidateRole::Chinese ||
                                 event.role == core::CommitCandidateRole::UserDictionary)) {
            recentChoices_.record(rawInput, candidate.text, candidate.fullPinyin, partialSelection);
        }
        appendFallbackContext(candidate.text);
        if (partialSelection) {
            auto remainder = rawInput.substr(candidate.consumedInputBytes);
            while (!remainder.empty() && remainder.front() == '\'') {
                remainder.erase(remainder.begin());
            }
            context->clear();
            if (!remainder.empty() && !context->type(remainder)) {
                context->clear();
                context->type(rawInput);
                refresh();
                return std::nullopt;
            }
            refresh();
            outcome.action = core::SelectionAction::CommitText;
            outcome.remainingInput = context->userInput();
            return outcome;
        }

        context->clear();
        refresh();
        outcome.action = core::SelectionAction::CommitText;
        outcome.remainingInput = context->userInput();
        return outcome;
    }

    bool select(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        core::CandidateSelectionRequest req;
        req.displayGeneration = page_.generation;
        req.globalIndex = index;
        req.expected = core::candidateIdentity(page_.items[index], page_);
        return selectConfirmed(req, page_.generation).has_value();
    }

    bool extractCharacter(std::size_t index, bool firstChar,
                          std::string *committedText) {
        if (checkResourceChanges()) {
            return false;
        }
        if (!pageAuthorized_) {
            return false;
        }
        if (shared_ && shared_->epochViewState() == core::EpochViewState::Reloading) {
            return false;
        }
        if (index >= page_.items.size()) {
            return false;
        }
        const auto &candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::None ||
            candidate.source == core::CandidateSource::Command ||
            candidate.source == core::CandidateSource::EnglishDefinition ||
            candidate.source == core::CandidateSource::Raw) {
            return false;
        }
        if (candidate.text.empty() || candidate.fullPinyin.empty()) {
            return false;
        }

        std::vector<std::string> utf8Chars;
        for (std::size_t i = 0; i < candidate.text.size(); ) {
            unsigned char c = static_cast<unsigned char>(candidate.text[i]);
            std::size_t len = 1;
            if ((c & 0x80) == 0) {
                return false;
            } else if ((c & 0xE0) == 0xC0) {
                return false;
            } else if ((c & 0xF0) == 0xE0) {
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                len = 4;
            } else {
                return false;
            }
            if (i + len > candidate.text.size()) {
                return false;
            }
            utf8Chars.push_back(candidate.text.substr(i, len));
            i += len;
        }

        std::string_view pinyinView = candidate.fullPinyin;
        while (!pinyinView.empty() && pinyinView.front() == '\'') {
            pinyinView.remove_prefix(1);
        }
        while (!pinyinView.empty() && pinyinView.back() == '\'') {
            pinyinView.remove_suffix(1);
        }
        if (pinyinView.empty()) {
            return false;
        }

        std::vector<std::string> syllables;
        std::size_t start = 0;
        while (start < pinyinView.size()) {
            auto pos = pinyinView.find('\'', start);
            if (pos == std::string_view::npos) {
                syllables.push_back(std::string(pinyinView.substr(start)));
                break;
            }
            if (pos > start) {
                syllables.push_back(std::string(pinyinView.substr(start, pos - start)));
            }
            start = pos + 1;
        }
        for (const auto &s : syllables) {
            if (s.empty()) {
                return false;
            }
        }

        if (utf8Chars.empty() || utf8Chars.size() != syllables.size()) {
            return false;
        }

        const auto chosenChar = firstChar ? utf8Chars.front() : utf8Chars.back();
        const auto chosenSyllable = firstChar ? syllables.front() : syllables.back();

        if (committedText != nullptr) {
            *committedText = chosenChar;
        }

        const auto rawInput = context->userInput();
        auto event = makeCommitEvent(
            chosenChar, chosenSyllable, chosenSyllable,
            core::CommitCandidateRole::Chinese, rawInput.size(),
            false, true, 0, 0, contextBefore_, contextAfter_);

        phraseTracker_.cancelComposition();
        core::CommitEvent genericEvent = event;
        genericEvent.contextBefore.clear();
        genericEvent.contextAfter.clear();
        for (int rep = 0; rep < 3; ++rep) {
            onCommitEvent(genericEvent);
        }
        if (!contextBefore_.empty() || !contextAfter_.empty()) {
            onCommitEvent(event);
        }
        appendFallbackContext(chosenChar);

        context->clear();
        refresh();
        return true;
    }

    bool remove(std::size_t index) {
        if (checkResourceChanges()) {
            return false;
        }
        if (!pageAuthorized_) {
            return false;
        }
        if (shared_ && shared_->epochViewState() == core::EpochViewState::Reloading) {
            return false;
        }
        if (index >= page_.items.size()) {
            return false;
        }
        const auto candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::None ||
            candidate.source == core::CandidateSource::EnglishDefinition ||
            candidate.source == core::CandidateSource::Command) {
            return false;
        }
        if (candidate.source == core::CandidateSource::Raw) {
            if (learningWriter() != nullptr) {
                const auto pinyinKey = candidate.fullPinyin.empty()
                                           ? std::string(page_.rawInput)
                                           : candidate.fullPinyin;
                learningWriter()->enqueueSuppression(candidate.text, pinyinKey);
                suppressedLearned_.insert(candidateKey(pinyinKey, candidate.text));
                refresh();
                return true;
            }
            return false;
        }
        const auto rawInput = context->userInput();
        if (candidate.source == core::CandidateSource::UserDictionary) {
            auto updatedDictionary = userDictionary();
            if (!updatedDictionary.remove(candidate.fullPinyin,
                                          candidate.text) ||
                !updatedDictionary.saveText(userDictionaryPath())) {
                return false;
            }
            const bool removedFromLibime = userDictionary().removeFrom(
                *ime().dict(), 1, candidate.fullPinyin, candidate.text);
            if (!removedFromLibime) {
                // Restore the original file if the in-memory dictionary layer
                // could not be changed.
                userDictionary().saveText(userDictionaryPath());
                return false;
            }
            userDictionary() = std::move(updatedDictionary);
            rebuildContext(rawInput);
            return true;
        }

        if (candidate.source == core::CandidateSource::Learned ||
            candidate.source == core::CandidateSource::Engine) {
            // Engine 源候选来自系统词典，无法像用户词典那样物理删除；
            // 记录本地兜底屏蔽，并在下方写入共享持久化快照；refresh()
            // 以快照为准，令其他输入上下文和重启后也一致隐藏此词。
            suppressedLearned_.insert(
                candidateKey(candidate.fullPinyin, candidate.text));
        }
        if (learningWriter() != nullptr) {
            learningWriter()->enqueueSuppression(candidate.text,
                                                 candidate.fullPinyin);
        }
        refresh();
        return true;
    }

    void appendFallbackContext(std::string_view text) {
        if (text.empty() || !contextLearningEnabled_ || !learningEnabled_) {
            return;
        }
        fallbackContextBefore_.append(text);
        constexpr std::size_t maxFallbackChars = 32;
        std::size_t charCount = 0;
        std::size_t end = fallbackContextBefore_.size();
        while (end > 0 && charCount < maxFallbackChars) {
            std::size_t start = end - 1;
            while (start > 0 && (static_cast<unsigned char>(fallbackContextBefore_[start]) & 0xc0U) == 0x80U) {
                --start;
            }
            end = start;
            ++charCount;
        }
        if (end > 0) {
            fallbackContextBefore_.erase(0, end);
        }
        contextBefore_ = fallbackContextBefore_;
    }

    void reset() {
        phraseTracker_.cancelComposition();
        contextBefore_ = fallbackContextBefore_;
        contextAfter_.clear();
        // 删词屏蔽跨输入组合持续生效：用户明确删除的词不应在下一个
        // composition 里复活，屏蔽集合生命周期与 provider 一致。
        context->clear();
        refresh();
    }

    bool flush() {
        if (auto *learning = learningWriter(); learning != nullptr) {
            return learning->flush();
        }
        return true;
    }

    void resetCommitHistory() {
        recentChoices_.clear();
        phraseTracker_.resetCommitHistory();
        fallbackContextBefore_.clear();
        contextBefore_.clear();
        contextAfter_.clear();
    }

    void setContext(std::string_view before, std::string_view after) {
        if (!contextLearningEnabled_ || !learningEnabled_) {
            contextBefore_.clear();
            contextAfter_.clear();
            fallbackContextBefore_.clear();
            return;
        }
        contextBefore_ = before;
        contextAfter_ = after;
        fallbackContextBefore_ = before;
    }

    void setLearningEnabled(bool enabled) {
        learningEnabled_ = enabled;
        if (!enabled) {
            recentChoices_.clear();
            // Shared normal-mode counts survive; only this context's unfinished
            // sequences and surrounding text are discarded at the boundary.
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            fallbackContextBefore_.clear();
            contextBefore_.clear();
            contextAfter_.clear();
        } else if (shared_) {
            shared_->prepareLearningWriterAsync();
        }
    }

    void setContextLearningEnabled(bool enabled) {
        contextLearningEnabled_ = enabled;
        if (!enabled) {
            contextBefore_.clear();
            contextAfter_.clear();
            fallbackContextBefore_.clear();
        }
    }

    void setEnglishDefinitionEnabled(bool enabled) {
        if (englishDefinitionEnabled_ != enabled) {
            englishDefinitionEnabled_ = enabled;
            refresh();
        }
    }

    void setClipboardEnabled(bool enabled) {
        if (clipboardEnabled_ != enabled) {
            clipboardEnabled_ = enabled;
            refresh();
        }
    }

    void setClipboardTrigger(std::string trigger) {
        if (clipboardTrigger_ != trigger) {
            clipboardTrigger_ = std::move(trigger);
            refresh();
        }
    }

    void applyOptions(const PinyinProviderOptions &opts) {
        bool needRefresh = false;
        if (englishDefinitionEnabled_ != opts.englishDefinitionEnabled) {
            englishDefinitionEnabled_ = opts.englishDefinitionEnabled;
            needRefresh = true;
        }
        if (clipboardEnabled_ != opts.clipboardEnabled) {
            clipboardEnabled_ = opts.clipboardEnabled;
            needRefresh = true;
        }
        if (clipboardTrigger_ != opts.clipboardTrigger) {
            clipboardTrigger_ = opts.clipboardTrigger;
            needRefresh = true;
        }
        setLearningEnabled(opts.learningEnabled);
        setContextLearningEnabled(opts.contextLearningEnabled);
        if (needRefresh) {
            refresh();
        }
    }

    const core::CandidatePage &page() const {
        if (const_cast<Impl *>(this)->checkResourceChanges() ||
            (!pageAuthorized_ && context && !context->userInput().empty())) {
            const_cast<Impl *>(this)->refresh();
        }
        return page_;
    }

    std::shared_ptr<SharedResources> sharedResources() const { return shared_; }

    void setOnReadyCallback(std::function<void()> callback) {
        if (shared_) {
            shared_->setOnReadyCallback(std::move(callback));
        }
    }

private:
    libime::PinyinIME &ime() { return *shared_->ime; }
    core::LearningWriter *learningWriter() {
        if (!learningEnabled_) {
            return nullptr;
        }
        return shared_->ensureLearningWriter(false);
    }
    UserDictionary &userDictionary() { return shared_->userDictionary; }
    const std::filesystem::path &userDictionaryPath() const {
        return shared_->userDictionaryPath;
    }

    void rebuildContext(const std::string &rawInput) {
        context->clear();
        if (!rawInput.empty()) {
            context->type(rawInput);
        }
        refresh();
    }

    void refresh() {
        if (checkResourceChanges()) {
            const auto currentInput = context->userInput();
            if (!currentInput.empty()) {
                context->clear();
                context->type(currentInput);
            }
        }
        std::vector<std::string> previousOrder;
        previousOrder.reserve(page_.items.size());
        for (const auto &item : page_.items) {
            previousOrder.push_back(
                core::candidateOrderKey(item.text, item.fullPinyin));
        }
        page_.clear();
        const auto rawInput = context->userInput();
        page_.rawInput = rawInput;
        page_.preedit = rawInput;
        ++generation;
        page_.generation = generation;
        if (rawInput.empty()) {
            pageAuthorized_ = false;
            return;
        }

        if (shared_ && shared_->epochViewState() == core::EpochViewState::Reloading) {
            core::CandidateItem item;
            item.text = "正在更新候选";
            item.source = core::CandidateSource::None;
            item.fullPinyin = rawInput;
            item.consumedInputBytes = 0;
            page_.items.push_back(std::move(item));
            pageAuthorized_ = false;
            return;
        }

        // Suppression is a visibility contract, independent of permission to
        // learn. Disabled/sensitive sessions may read tombstones, never enqueue
        // selections or use personal scoring features through this snapshot.
        const auto visibility = shared_->visibleLearning();
        const auto learning = learningEnabled_ ? visibility : nullptr;
        const auto isSuppressed = [&](std::string_view text,
                                      std::string_view pinyin) {
            // A shared positive selection or external clear can remove a
            // tombstone. Prefer the live snapshot over a stale context mask.
            return visibility != nullptr
                       ? visibility->isSuppressed(text, pinyin)
                       : suppressedLearned_.contains(candidateKey(pinyin, text));
        };
        std::unique_ptr<libime::PinyinContext> typoContext;
        const auto correctedInput = correctTypoInput(rawInput);
        if (correctedInput != rawInput) {
            try {
                typoContext =
                    std::make_unique<libime::PinyinContext>(shared_->ime.get());
                typoContext->type(correctedInput);
            } catch (const std::exception &) {
                typoContext.reset();
            }
        }
        // 容错变体上下文（2026-10-24 容错专项 F3/F4/F6/F8）：对有效拼音串
        // 构造近音变体解码。门槛：无隔音符、长度 >= 3、且不是单个合法音节
        // （de/qu/zhang 等单音节不做变体，避免高频输入的噪声与额外解码）。
        std::unique_ptr<libime::PinyinContext> variantContext;
        std::string variantInput;
        if (rawInput.size() >= 3 && rawInput.find('\'') == std::string::npos &&
            !core::PinyinMatchPolicy::isStandardSyllable(rawInput)) {
            variantInput = core::PinyinMatchPolicy::rhymeVariantInput(rawInput);
            if (variantInput != rawInput) {
                try {
                    variantContext =
                        std::make_unique<libime::PinyinContext>(shared_->ime.get());
                    variantContext->type(variantInput);
                } catch (const std::exception &) {
                    variantContext.reset();
                    variantInput.clear();
                }
            }
        }
        const auto requestNowMs = nowMilliseconds();
        const auto result = buildCandidatePipeline(
            *context, *ime().dict(), learning.get(), requestNowMs,
            contextBefore_, contextAfter_, previousOrder, typoContext.get(), &recentChoices_,
            variantContext.get());
        const auto segmentedInput =
            automaticallySegmentedPreedit(rawInput, result);
        const auto prefixEnds = syllablePrefixEnds(rawInput, segmentedInput);
        std::vector<std::pair<std::size_t, std::string>> canonicalPrefixes;
        canonicalPrefixes.reserve(prefixEnds.size());
        for (const auto prefixEnd : prefixEnds) {
            canonicalPrefixes.emplace_back(
                prefixEnd,
                core::PinyinMatchPolicy::canonical(prefixInput(rawInput, prefixEnd)));
        }
        page_.items.reserve(result.order.size() + 4);
        // canonical(userInput) 对全部候选相同，循环外算一次传入，
        // 避免每个候选重复做一次规范化字符串分配。
        const auto canonicalInput =
            core::PinyinMatchPolicy::canonical(rawInput);
        // 简拼/混拼消费范围还原（2026-10-25 用户实机 z'm'hui'shi 事故）：
        // 输入音节段与候选音节的逐段对齐依据，循环外切好一次。
        std::vector<std::string> inputSegments;
        for (std::size_t start = 0; start < segmentedInput.size();) {
            const auto sep = segmentedInput.find('\'', start);
            const auto stop = sep == std::string::npos ? segmentedInput.size() : sep;
            if (stop > start) {
                inputSegments.emplace_back(segmentedInput.substr(start, stop - start));
            }
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        const bool hasPinyinCoverage = std::any_of(
            result.scored.begin(), result.scored.end(),
            [&rawInput, &canonicalInput](const auto &candidate) {
                return coversPinyinInput(rawInput, canonicalInput,
                                         candidate.full_pinyin);
            });
        const auto manualLimit =
            pinyinLetterCount(rawInput) < 3 ? std::size_t{2} : std::size_t{8};
        std::size_t manualCount = 0;
        std::unordered_set<std::string> seen;
        seen.reserve(result.order.size());
        std::vector<core::CandidateItem> fullItems;
        fullItems.reserve(result.order.size());
        std::vector<core::CandidateItem> partialPool;
        partialPool.reserve(result.order.size());
        for (const auto sourceIndex : result.order) {
            const auto &candidate = result.scored[sourceIndex];
            const bool isManual = userDictionary().contains(
                candidate.full_pinyin, candidate.text);
            auto key = candidateKey(candidate.full_pinyin, candidate.text);
            if (isSuppressed(candidate.text, candidate.full_pinyin)) {
                continue;
            }
            const bool isLearned = !isManual && learning != nullptr &&
                                   learning->hasPositiveFrequency(
                                       candidate.text, candidate.full_pinyin);
            if (isManual && manualCount >= manualLimit) {
                continue;
            }
            if (!seen.insert(std::move(key)).second) {
                continue;
            }
            core::CandidateItem item;
            item.text = candidate.text;
            item.fullPinyin = candidate.full_pinyin;
            item.sourceIndex = candidate.source_index;
            item.learningBoost = candidate.learning_boost;
            item.recentChoiceBonus = candidate.recent_choice_bonus;
            item.isLexical = candidate.is_lexical || candidate.dictionary_bonus > 0.0;
            item.source = isManual
                              ? core::CandidateSource::UserDictionary
                              : (isLearned ? core::CandidateSource::Learned
                                           : core::CandidateSource::Engine);
            item.commitRole = isManual
                                  ? core::CommitCandidateRole::UserDictionary
                                  : core::CommitCandidateRole::Chinese;
            item.origins = {item.source};
            if (isLearned && item.source != core::CandidateSource::Engine) {
                item.origins.push_back(core::CandidateSource::Engine);
            }
            if (isManual) {
                ++manualCount;
            }
            item.consumedInputBytes = rawInput.size();
            const auto candidatePriority = core::PinyinMatchPolicy::priority(
                rawInput, candidate.full_pinyin);
            if (candidatePriority == -1) {
                const auto candidatePinyin =
                    core::PinyinMatchPolicy::canonical(candidate.full_pinyin);
                for (const auto &[prefixEnd, canonicalPrefix] : canonicalPrefixes) {
                    if (canonicalPrefix == candidatePinyin) {
                        item.consumedInputBytes = prefixEnd;
                        break;
                    }
                }
            } else if (candidatePriority == 0 && !inputSegments.empty() &&
                       !prefixEnds.empty()) {
                // 简拼/混拼下的部分消费候选（如 zmhuishi 下的 怎么=zm）此前
                // 被默认标成全消费：高频学习词条借加成越过整句篡位，且选词
                // 会吞掉未消费后缀。按"输入音节段是候选音节的前缀（声母、
                // zh/ch/sh 双字母声母或全音节）"逐段对齐求最大前缀；候选音
                // 节全部对齐且仍短于输入时，还原为真实前缀消费。整词覆盖
                // （对齐到输入末尾）保持全消费不变，-1 精确前缀路径不变。
                // 持有拷贝：full_pinyin.substr 返回临时 std::string，转
                // string_view 存向量会悬垂（本轮实测教训）。
                std::vector<std::string> candidateSyllables;
                for (std::size_t start = 0; start < candidate.full_pinyin.size();) {
                    const auto sep = candidate.full_pinyin.find('\'', start);
                    const auto stop = sep == std::string::npos
                                          ? candidate.full_pinyin.size() : sep;
                    if (stop > start) {
                        candidateSyllables.emplace_back(
                            candidate.full_pinyin.substr(start, stop - start));
                    }
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
                if (!candidateSyllables.empty() &&
                    candidateSyllables.size() <= inputSegments.size()) {
                    std::size_t matched = 0;
                    for (; matched < candidateSyllables.size(); ++matched) {
                        if (matched >= inputSegments.size() ||
                            !candidateSyllables[matched].starts_with(
                                inputSegments[matched])) {
                            break;
                        }
                    }
                    if (matched == candidateSyllables.size() &&
                        matched < inputSegments.size() &&
                        matched <= prefixEnds.size()) {
                        item.consumedInputBytes = matched == 0
                                                      ? 0
                                                      : prefixEnds[matched - 1];
                    }
                }
            }
            if (item.consumedInputBytes < rawInput.size()) {
                partialPool.push_back(std::move(item));
            } else {
                fullItems.push_back(std::move(item));
            }
        }

        const bool isInitialConsonant = (rawInput == "sh" || rawInput == "ch" || rawInput == "zh");
        const bool isConsonantAbbreviation =
            rawInput.size() >= 2 && !fullItems.empty() &&
            std::all_of(rawInput.begin(), rawInput.end(), [len = rawInput.size()](char c) {
                return c >= 'a' && c <= 'z' &&
                       c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' && c != 'v' &&
                       (c != 'y' || len == 3);
            }) &&
            (rawInput.size() == 2 ||
             core::PinyinMatchPolicy::abbreviationKey(
                 fullItems.front().fullPinyin) == rawInput ||
             core::PinyinMatchPolicy::initialsMatch(
                 rawInput, fullItems.front().fullPinyin));
        const bool isEnglish =
            !isInitialConsonant && !isConsonantAbbreviation &&
            core::EnglishDictionary::isEnglishWord(rawInput);

        const bool hasExactFullPinyinMatch = std::any_of(
            fullItems.begin(), fullItems.end(),
            [&rawInput](const auto &item) {
                return item.source != core::CandidateSource::Raw &&
                       !hasInvalidInternalPinyinSegment(item.fullPinyin) &&
                       (core::PinyinMatchPolicy::exactInputMatch(
                            rawInput, item.fullPinyin) ||
                        core::PinyinMatchPolicy::isFullTypoMatch(
                            rawInput, item.fullPinyin));
            });

        const bool hasTrustedShortAbbreviation =
            std::any_of(fullItems.begin(), fullItems.end(),
                        [&rawInput](const auto &item) {
                            return item.source != core::CandidateSource::Raw &&
                                   core::PinyinMatchPolicy::
                                       trustedShortAbbreviationMatch(
                                           rawInput, item.fullPinyin,
                                           item.text);
                        });

        bool isTopChineseLexical = false;
        if (!fullItems.empty()) {
            const auto &topChinese = fullItems.front();
            if (topChinese.source == core::CandidateSource::UserDictionary ||
                topChinese.isLexical ||
                (utf8CodePointCount(topChinese.text) == 1 && rawInput.size() <= 3)) {
                isTopChineseLexical = true;
            } else {
                try {
                    std::string cleanPinyin = topChinese.fullPinyin;
                    while (!cleanPinyin.empty() && cleanPinyin.back() == '\'') {
                        cleanPinyin.pop_back();
                    }
                    if (!cleanPinyin.empty()) {
                        const auto encoded = libime::PinyinEncoder::encodeFullPinyin(cleanPinyin);
                        ime().dict()->matchWords(encoded.data(), encoded.size(), [&](std::string_view, std::string_view hanzi, float) {
                            if (hanzi == topChinese.text) {
                                isTopChineseLexical = true;
                                return false;
                            }
                            return true;
                        });
                    }
                } catch (const std::exception &) {
                    isTopChineseLexical = false;
                }
            }
        }

        const bool hasHighConfidenceChinese =
            !fullItems.empty() &&
            (hasTrustedShortAbbreviation || isConsonantAbbreviation ||
             (hasExactFullPinyinMatch && isTopChineseLexical));

        // 单音节完整拼音输入（you/can/she 等）在存在精确匹配的高置信中文时，
        // 不注入静态英文补全：单音节前排必须保持全部地道中文（既有单音节
        // 英文沉底契约）。已学英文召回与纯声母缩写（pyt→python）不受此限制。
        const bool singleSyllableChineseProtected =
            rawInput.find('\'') == std::string::npos &&
            core::PinyinMatchPolicy::isValidPinyinSequence(rawInput) &&
            core::PinyinMatchPolicy::segmentPinyin(rawInput).find('\'') ==
                std::string::npos &&
            hasExactFullPinyinMatch && isTopChineseLexical;

        const bool shouldPredictEnglish =
            (rawInput.empty() || rawInput.front() != 'v') &&
            !hasTrustedShortAbbreviation &&
            rawInput.size() >= 3 &&
            !singleSyllableChineseProtected &&
            (isEnglish ||
             (isConsonantAbbreviation && rawInput.size() <= 3) ||
             (!hasHighConfidenceChinese && !hasExactFullPinyinMatch &&
              (!hasPinyinCoverage || hasInvalidInternalPinyinSegment(segmentedInput))));

        std::vector<std::string> predictedWords;
        std::size_t learnedEnglishCount = 0;
        const bool canPredictEnglish =
            (rawInput.empty() || rawInput.front() != 'v') &&
            !hasTrustedShortAbbreviation &&
            rawInput.size() >= 2;

        if (canPredictEnglish && learning != nullptr) {
            const auto learned = learning->recallEnglishWords(rawInput, 3, nowMilliseconds());
            for (const auto &item : learned) {
                if (!isSuppressed(item.text, item.text)) {
                    predictedWords.push_back(item.text);
                    ++learnedEnglishCount;
                }
            }
        }
        if (shouldPredictEnglish) {
            const auto staticWords = core::EnglishDictionary::predictWords(rawInput, 3);
            for (const auto &w : staticWords) {
                if (isConsonantAbbreviation && !core::EnglishDictionary::isModernTechWord(w)) {
                    continue;
                }
                if (!isSuppressed(w, w) &&
                    std::find(predictedWords.begin(), predictedWords.end(), w) == predictedWords.end()) {
                    predictedWords.emplace_back(w);
                }
            }
        }

        const auto *bestFullSentence =
            fullItems.empty() ? nullptr : &fullItems.front();
        auto prefixEvidence = bestFullSentence != nullptr
            ? buildPrefixEvidence(*bestFullSentence, prefixEnds, rawInput,
                                  partialPool, learning.get(), requestNowMs, &recentChoices_)
            : std::vector<core::CandidateItem>{};

        // 变体召回的最佳候选交给组合器占第 2 位可见席位（含读音标注依据）。
        std::optional<core::CandidateItem> compInputVariantTop;
        if (variantContext != nullptr && !variantInput.empty()) {
            const auto variantCanonical =
                core::PinyinMatchPolicy::canonical(variantInput);
            for (const auto &item : fullItems) {
                if (item.isLexical && item.consumedInputBytes == rawInput.size() &&
                    core::PinyinMatchPolicy::canonical(item.fullPinyin) ==
                        variantCanonical) {
                    compInputVariantTop = item;
                    break;
                }
            }
        }

        CandidateCompositionInput compInput;
        compInput.rawInput = rawInput;
        compInput.canonicalInput = canonicalInput;
        compInput.segmentedInput = segmentedInput;
        compInput.fullCandidates = std::move(fullItems);
        compInput.partialCandidates = std::move(partialPool);
        compInput.prefixEnds = prefixEnds;
        compInput.prefixEvidence = std::move(prefixEvidence);
        compInput.variantTopCandidate = std::move(compInputVariantTop);
        compInput.macroCandidates = generateMacroCandidates(rawInput, canonicalInput);
        compInput.englishPredictions = std::move(predictedWords);
        compInput.learnedEnglishCount = learnedEnglishCount;
        compInput.isEnglishWord = isEnglish;
        compInput.hasPinyinCoverage = hasPinyinCoverage;
        compInput.hasInvalidInternalPinyinSegment = hasInvalidInternalPinyinSegment(segmentedInput);
        compInput.clipboardEnabled = clipboardEnabled_;
        compInput.clipboardTrigger = clipboardTrigger_;
        if (englishDefinitionEnabled_ && shared_) {
            compInput.resolveEnglishDefinition = [this](std::string_view word) -> std::string {
                if (!englishDefinitionEnabled_ || !shared_ || word.size() < 4) {
                    return {};
                }
                auto *dict = shared_->ensureEnglishDefinitionDictionary();
                if (dict == nullptr) {
                    return {};
                }
                const auto rawDef = dict->lookup(word);
                if (rawDef.empty()) {
                    return {};
                }
                return core::EnglishDefinitionDictionary::cleanDefinition(rawDef, 12);
            };
        }
        compInput.topChineseLearningBoost =
            (!compInput.fullCandidates.empty() && compInput.fullCandidates.front().learningBoost > 0.0)
                ? compInput.fullCandidates.front().learningBoost
                : (learning != nullptr && !compInput.fullCandidates.empty()
                       ? learning->boostAt(compInput.fullCandidates.front().text,
                                           compInput.fullCandidates.front().fullPinyin,
                                           nowMilliseconds())
                       : 0.0);
        compInput.isSuppressed = isSuppressed;
        compInput.isLexicalWord = [this](std::string_view text, std::string_view pinyin) {
            try {
                std::string cleanPinyin(pinyin);
                while (!cleanPinyin.empty() && cleanPinyin.back() == '\'') {
                    cleanPinyin.pop_back();
                }
                if (cleanPinyin.empty()) return false;
                const auto encoded = libime::PinyinEncoder::encodeFullPinyin(cleanPinyin);
                bool matched = false;
                ime().dict()->matchWords(encoded.data(), encoded.size(), [&](std::string_view, std::string_view hanzi, float) {
                    if (hanzi == text) {
                        matched = true;
                        return false;
                    }
                    return true;
                });
                return matched;
            } catch (const std::exception &) {
                return false;
            }
        };

        auto compResult = composeCandidates(compInput);
        page_.items = std::move(compResult.items);
        page_.preedit = std::move(compResult.preedit);
        // 内联读音提示（2026-10-25 调研后重做，规则同主流输入法"读音有误
        // 才标注"）：仅当用户以全拼逐字输入、且标注位的实际读音与该字在
        // 本词中的规范读音不一致（如把 卡住 记成 kazhu，规范为 qia）时，
        // 在该字后内联标注正确读音（渲染为 卡qiǎ住）。词表离线经 Unihan
        // 逐字校验；简拼、错位拼、正确读音输入一律不提示；标注只属展示层，
        // 不改身份、排序与消费范围。
        {
            // 持有拷贝而非 string_view：segmented 是临时串，视图会悬垂。
            std::vector<std::string> rawSyllables;
            {
                const auto segmented =
                    core::PinyinMatchPolicy::segmentPinyin(rawInput);
                for (std::size_t start = 0; start < segmented.size();) {
                    const auto sep = segmented.find('\'', start);
                    const auto stop = sep == std::string::npos ? segmented.size() : sep;
                    if (stop > start) {
                        rawSyllables.emplace_back(segmented.substr(start, stop - start));
                    }
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
            }
            bool rawIsFullPinyin = !rawSyllables.empty();
            for (const auto &syllable : rawSyllables) {
                if (!core::PinyinMatchPolicy::isStandardSyllable(syllable)) {
                    rawIsFullPinyin = false;
                    break;
                }
            }
            std::size_t hinted = 0;
            for (std::size_t i = 0; i < page_.items.size() && i < 3 && hinted < 2; ++i) {
                auto &item = page_.items[i];
                if (!rawIsFullPinyin || !item.isLexical ||
                    item.source == core::CandidateSource::Raw ||
                    item.source == core::CandidateSource::Command ||
                    item.source == core::CandidateSource::EnglishDefinition) {
                    continue;
                }
                const auto *hint = core::readingHintFor(item.text);
                if (hint == nullptr) {
                    continue;
                }
                std::size_t charCount = 0;
                for (const char c : item.text) {
                    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++charCount;
                }
                if (charCount == 0 || rawSyllables.size() != charCount ||
                    hint->afterCharIndex >= charCount) {
                    continue; // 输入音节与整词字数不对应（简拼/部分消费）
                }
                std::vector<std::string_view> wordSyllables;
                {
                    std::string_view pinyin = item.fullPinyin;
                    if (pinyin.starts_with("!u!") || pinyin.starts_with("!U!")) {
                        pinyin.remove_prefix(3);
                    }
                    for (std::size_t start = 0; start < pinyin.size();) {
                        const auto sep = pinyin.find('\'', start);
                        const auto stop = sep == std::string_view::npos ? pinyin.size() : sep;
                        if (stop > start) {
                            wordSyllables.emplace_back(pinyin.substr(start, stop - start));
                        }
                        if (sep == std::string_view::npos) break;
                        start = sep + 1;
                    }
                }
                if (wordSyllables.size() != charCount) {
                    continue;
                }
                bool sameWordTyping = true;
                for (std::size_t s = 0; s < charCount; ++s) {
                    if (s == hint->afterCharIndex) continue;
                    if (rawSyllables[s] != wordSyllables[s]) {
                        sameWordTyping = false;
                        break;
                    }
                }
                if (!sameWordTyping) {
                    continue;
                }
                if (rawSyllables[hint->afterCharIndex] ==
                    std::string_view(hint->canonicalToneless)) {
                    continue; // 该字读音输入正确，不提示
                }
                item.readingHint = hint->reading;
                item.readingHintAfterCharIndex = hint->afterCharIndex;
                ++hinted;
            }
        }
        pageAuthorized_ = !page_.items.empty();
    }

    std::shared_ptr<SharedResources> shared_;
    core::PhraseFormationTracker phraseTracker_;
    RecentChoices recentChoices_;
    std::unique_ptr<libime::PinyinContext> context;
    bool contextLearningEnabled_ = true;
    bool learningEnabled_ = true;
    bool englishDefinitionEnabled_ = false;
    bool clipboardEnabled_ = true;
    std::string clipboardTrigger_ = "V+2";
    core::CandidatePage page_;
    std::string contextBefore_;
    std::string contextAfter_;
    std::string fallbackContextBefore_;
    std::unordered_set<std::string> suppressedLearned_;
    std::uint64_t generation = 0;
    bool pageAuthorized_ = false;
    std::uint64_t observedDictionaryGeneration_ = 0;
    std::int64_t observedLearningRevision_ = 0;
    core::EpochViewState observedEpochViewState_ = core::EpochViewState::Ready;
};

PinyinCandidateProvider::PinyinCandidateProvider(PinyinDataPaths paths,
                                                 PinyinProviderOptions options)
    : impl_(std::make_unique<Impl>(paths, options)) {}

PinyinCandidateProvider::PinyinCandidateProvider(
    std::shared_ptr<SharedResources> shared, PinyinProviderOptions options)
    : impl_(std::make_unique<Impl>(std::move(shared), options)) {}

PinyinCandidateProvider::~PinyinCandidateProvider() = default;

bool PinyinCandidateProvider::append(std::string_view input) {
    return impl_->append(input);
}

bool PinyinCandidateProvider::eraseLast() { return impl_->eraseLast(); }

bool PinyinCandidateProvider::replaceInput(std::string_view input) {
    return impl_->replaceInput(input);
}

bool PinyinCandidateProvider::select(std::size_t index) {
    return impl_->select(index);
}

std::optional<core::CandidateSelectionOutcome> PinyinCandidateProvider::selectConfirmed(
    const core::CandidateSelectionRequest &request,
    std::uint64_t providerGeneration) {
    return impl_->selectConfirmed(request, providerGeneration);
}

bool PinyinCandidateProvider::extractCharacter(std::size_t index, bool firstChar,
                                               std::string *outText) {
    return impl_->extractCharacter(index, firstChar, outText);
}

bool PinyinCandidateProvider::remove(std::size_t index) {
    return impl_->remove(index);
}

void PinyinCandidateProvider::onCommitEvent(const core::CommitEvent &event) {
    impl_->onCommitEvent(event);
}

void PinyinCandidateProvider::onNegativeFeedback(std::string_view phrase,
                                                 std::string_view pinyin) {
    impl_->onNegativeFeedback(phrase, pinyin);
}

void PinyinCandidateProvider::reset() { impl_->reset(); }

void PinyinCandidateProvider::resetCommitHistory() {
    impl_->resetCommitHistory();
}

bool PinyinCandidateProvider::flush() {
    return impl_->flush();
}

const core::CandidatePage &PinyinCandidateProvider::page() const {
    return impl_->page();
}

void PinyinCandidateProvider::setContext(std::string_view before,
                                         std::string_view after) {
    impl_->setContext(before, after);
}

void PinyinCandidateProvider::applyOptions(const PinyinProviderOptions &options) {
    impl_->applyOptions(options);
}

void PinyinCandidateProvider::setLearningEnabled(bool enabled) {
    impl_->setLearningEnabled(enabled);
}

void PinyinCandidateProvider::setContextLearningEnabled(bool enabled) {
    impl_->setContextLearningEnabled(enabled);
}

void PinyinCandidateProvider::setEnglishDefinitionEnabled(bool enabled) {
    impl_->setEnglishDefinitionEnabled(enabled);
}

void PinyinCandidateProvider::setClipboardEnabled(bool enabled) {
    impl_->setClipboardEnabled(enabled);
}

void PinyinCandidateProvider::setClipboardTrigger(std::string trigger) {
    impl_->setClipboardTrigger(std::move(trigger));
}

std::shared_ptr<PinyinCandidateProvider::SharedResources>
PinyinCandidateProvider::sharedResources() const {
    return impl_->sharedResources();
}

void PinyinCandidateProvider::setOnReadyCallback(std::function<void()> callback) {
    impl_->setOnReadyCallback(std::move(callback));
}

} // namespace modernime::pinyin
