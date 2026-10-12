#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/pinyin/candidate_mixer.h"
#include "modernime/pinyin/candidate_pipeline.h"
#include "modernime/pinyin/user_dictionary.h"

#include "modernime/core/pinyin_match.h"
#include "modernime/core/english_dictionary.h"
#include "modernime/core/english_definition_dictionary.h"
#include "modernime/core/learning_writer.h"
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

std::filesystem::path defaultLearningPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .learningStore;
}

std::filesystem::path defaultUserDictionaryPath() {
    const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    const auto *home = std::getenv("HOME");
    return core::SettingsPaths::fromEnvironment(
               xdgConfigHome == nullptr ? std::string_view{}
                                        : std::string_view(xdgConfigHome),
               dataHome == nullptr ? std::string_view{}
                                   : std::string_view(dataHome),
               home == nullptr ? std::string_view{}
                               : std::string_view(home))
        .userDictionary;
}

bool isRegularFile(const std::filesystem::path &path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

void appendDataDirectoryCandidates(
    std::vector<std::filesystem::path> &candidates, std::string_view filename) {
    const auto *environment = std::getenv("XDG_DATA_DIRS");
    const std::string_view directories =
        environment != nullptr && *environment != '\0'
            ? std::string_view(environment)
            : std::string_view("/usr/local/share:/usr/share");
    std::size_t start = 0;
    while (start <= directories.size()) {
        const auto end = directories.find(':', start);
        const auto directory = directories.substr(start, end - start);
        const std::filesystem::path path(directory);
        if (path.is_absolute()) {
            candidates.emplace_back(path / "modernime" / "pinyin" / filename);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

std::filesystem::path extensionDictionaryPath(
    std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_PINYIN_KNOWLEDGE_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-knowledge.dict");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-knowledge.dict");
    }

    appendDataDirectoryCandidates(candidates, "modernime-knowledge.dict");

#ifdef MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_INSTALL_BINARY);
#endif
#ifdef MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-knowledge.dict");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path hotwordDictionaryPath(std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_PINYIN_HOTWORDS_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-hotwords.dict");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-hotwords.dict");
    }

    appendDataDirectoryCandidates(candidates, "modernime-hotwords.dict");

#ifdef MODERNIME_PINYIN_HOTWORDS_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_HOTWORDS_INSTALL_BINARY);
#endif
#ifdef MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY
    candidates.emplace_back(MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-hotwords.dict");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::filesystem::path englishDefinitionDictionaryPath(
    std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        return std::filesystem::path(configuredPath);
    }

    std::vector<std::filesystem::path> candidates;
    if (const auto *environment = std::getenv(
            "MODERNIME_ENGLISH_DEFINITION_DICTIONARY");
        environment != nullptr && *environment != '\0') {
        candidates.emplace_back(environment);
    }

    const auto *dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') {
        candidates.emplace_back(std::filesystem::path(dataHome) / "modernime" /
                                "pinyin" / "modernime-english-dict.bin");
    } else if (const auto *home = std::getenv("HOME"); home != nullptr &&
               *home != '\0') {
        candidates.emplace_back(std::filesystem::path(home) / ".local" /
                                "share" / "modernime" / "pinyin" /
                                "modernime-english-dict.bin");
    }

    appendDataDirectoryCandidates(candidates, "modernime-english-dict.bin");

#ifdef MODERNIME_ENGLISH_DICT_INSTALL_BINARY
    candidates.emplace_back(MODERNIME_ENGLISH_DICT_INSTALL_BINARY);
#endif
#ifdef MODERNIME_ENGLISH_DICT_SOURCE_BINARY
    candidates.emplace_back(MODERNIME_ENGLISH_DICT_SOURCE_BINARY);
#endif
    candidates.emplace_back(
        "/usr/share/modernime/pinyin/modernime-english-dict.bin");

    for (const auto &candidate : candidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

void loadExtensionDictionary(libime::PinyinDictionary &dictionary,
                             const std::filesystem::path &path) {
    if (!isRegularFile(path)) {
        return;
    }

    const auto index = dictionary.dictSize();
    dictionary.addEmptyDict();
    try {
        dictionary.load(index, path.c_str(), libime::PinyinDictFormat::Binary);
    } catch (...) {
        // An optional data package must never prevent the system dictionary
        // and user dictionary from starting.
        dictionary.clear(index);
    }
}

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

bool hasInvalidInternalPinyinSegment(std::string_view segmentedInput) {
    if (segmentedInput.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start < segmentedInput.size()) {
        const auto pos = segmentedInput.find('\'', start);
        if (pos == std::string_view::npos) {
            break;
        }
        const auto segment = segmentedInput.substr(start, pos - start);
        start = pos + 1;
        if (segment.empty()) {
            continue;
        }
        const bool hasVowel =
            std::any_of(segment.begin(), segment.end(), [](char c) {
                return c == 'a' || c == 'e' || c == 'i' || c == 'o' ||
                       c == 'u' || c == 'v';
            });
        if (!hasVowel) {
            return true;
        }
    }
    return false;
}

std::size_t utf8CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char character : text) {
        if ((character & 0xc0U) != 0x80U) {
            ++count;
        }
    }
    return count;
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

std::size_t pinyinSyllableCount(std::string_view fullPinyin) {
    std::string_view trimmed = fullPinyin;
    while (!trimmed.empty() && trimmed.back() == '\'') {
        trimmed.remove_suffix(1);
    }
    if (trimmed.empty()) {
        return 0;
    }
    return static_cast<std::size_t>(
               std::count(trimmed.begin(), trimmed.end(), '\'')) +
           1;
}

// Preserve the strongest exact conversion at index zero. If LibIME's native
// order ranks a fuzzy full conversion ahead of that exact conversion, place
// the fuzzy item second so low-value exact homophone variants follow it. Move
// the complete CandidateItem so selection metadata remains intact.
void interleaveHigherNativeRankFuzzyCandidate(
    std::string_view rawInput,
    std::vector<core::CandidateItem> &fullCandidates) {
    if (fullCandidates.size() < 2 ||
        core::PinyinMatchPolicy::priority(
            rawInput, fullCandidates.front().fullPinyin) != 2) {
        return;
    }

    // Only allow fuzzy candidate interleaving for multi-syllable input (>= 2 syllables).
    // Single-syllable inputs (e.g. cang, cao, cai, bin) must strictly preserve exact homophone matches,
    // and never interleave fuzzy homophones (e.g. chang/常, chao/超, chai/柴, bing/并) into rank 1.
    if (pinyinSyllableCount(fullCandidates.front().fullPinyin) <= 1) {
        return;
    }

    const auto fuzzy = std::find_if(
        std::next(fullCandidates.begin()), fullCandidates.end(),
        [&rawInput, &fullCandidates](const auto &candidate) {
            return core::PinyinMatchPolicy::priority(
                       rawInput, candidate.fullPinyin) == 0 &&
                   candidate.sourceIndex < fullCandidates.front().sourceIndex;
        });
    if (fuzzy == fullCandidates.end()) {
        return;
    }

    auto promoted = std::move(*fuzzy);
    fullCandidates.erase(fuzzy);
    fullCandidates.insert(std::next(fullCandidates.begin()),
                          std::move(promoted));
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

std::string defaultDictionaryPath() {
    if (const auto *env = std::getenv("MODERNIME_PINYIN_DICT");
        env != nullptr && *env != '\0') {
        return env;
    }
    if (const auto *env = std::getenv("LIBIME_PINYIN_DICT");
        env != nullptr && *env != '\0') {
        return env;
    }

    static const char *const kCandidates[] = {
        "/usr/share/libime/sc.dict",
        "/usr/lib64/libime/sc.dict",
        "/usr/lib/x86_64-linux-gnu/libime/sc.dict",
        "/usr/lib/aarch64-linux-gnu/libime/sc.dict",
        "/usr/lib/riscv64-linux-gnu/libime/sc.dict",
        "/usr/lib/loongarch64-linux-gnu/libime/sc.dict",
        "/usr/lib/libime/sc.dict",
        "/usr/local/share/libime/sc.dict",
        "/usr/local/lib64/libime/sc.dict",
        "/usr/local/lib/libime/sc.dict",
    };

    for (const char *candidate : kCandidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }
    return "/usr/share/libime/sc.dict";
}

std::string defaultLanguageModelPath() {
    if (const auto *env = std::getenv("MODERNIME_PINYIN_LM");
        env != nullptr && *env != '\0') {
        return env;
    }
    if (const auto *env = std::getenv("LIBIME_PINYIN_LM");
        env != nullptr && *env != '\0') {
        return env;
    }

    static const char *const kCandidates[] = {
        "/usr/lib64/libime/zh_CN.lm",
        "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/aarch64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/riscv64-linux-gnu/libime/zh_CN.lm",
        "/usr/lib/loongarch64-linux-gnu/libime/zh_CN.lm",
        "/usr/share/libime/zh_CN.lm",
        "/usr/lib/libime/zh_CN.lm",
        "/usr/local/lib64/libime/zh_CN.lm",
        "/usr/local/lib/libime/zh_CN.lm",
        "/usr/local/share/libime/zh_CN.lm",
    };

    for (const char *candidate : kCandidates) {
        if (isRegularFile(candidate)) {
            return candidate;
        }
    }

    std::error_code ec;
    if (std::filesystem::is_directory("/usr/lib64", ec)) {
        return "/usr/lib64/libime/zh_CN.lm";
    }
    return "/usr/lib/x86_64-linux-gnu/libime/zh_CN.lm";
}

class PinyinCandidateProvider::SharedResources final {
public:
    std::unique_ptr<libime::PinyinIME> ime;
    std::unique_ptr<core::LearningWriter> learning;
    UserDictionary userDictionary;
    std::filesystem::path userDictionaryPath;
    std::filesystem::path learningStorePath;
    core::EnglishDefinitionDictionary englishDefinitionDict;
    std::filesystem::path englishDefinitionDictPath;
    std::shared_ptr<core::PhraseFormationStats> phraseStats =
        std::make_shared<core::PhraseFormationStats>();

    core::LearningWriter *ensureLearningWriter() {
        if (learning == nullptr && !learningStorePath.empty()) {
            learning =
                std::make_unique<core::LearningWriter>(learningStorePath);
        }
        return learning.get();
    }

    core::EnglishDefinitionDictionary *ensureEnglishDefinitionDictionary() {
        if (!englishDefinitionDict.isLoaded() && !englishDefinitionDictPath.empty()) {
            englishDefinitionDict.load(englishDefinitionDictPath);
        }
        return englishDefinitionDict.isLoaded() ? &englishDefinitionDict : nullptr;
    }

    // Re-reads the user dictionary file and swaps the libime dictionary
    // layer, so settings-client edits apply without restarting fcitx5.
    bool reloadUserDictionary() {
        auto updated = UserDictionary::loadText(userDictionaryPath);
        ime->dict()->clear(1);
        updated.addTo(*ime->dict(), 1);
        userDictionary = std::move(updated);
        return true;
    }

    bool reloadLearningStore() {
        if (phraseStats != nullptr) {
            phraseStats->clear();
        }
        if (learning != nullptr) {
            return learning->reload();
        }
        return false;
    }
};

std::shared_ptr<PinyinCandidateProvider::SharedResources>
PinyinCandidateProvider::createSharedResources(
    const PinyinDataPaths &paths, const PinyinProviderOptions &options) {
    auto resources = std::make_shared<SharedResources>();
    resources->userDictionaryPath =
        paths.userDictionary.empty()
            ? defaultUserDictionaryPath()
            : std::filesystem::path(paths.userDictionary);
    resources->userDictionary =
        UserDictionary::loadText(resources->userDictionaryPath);
    resources->learningStorePath =
        paths.learningStore.empty()
            ? defaultLearningPath()
            : std::filesystem::path(paths.learningStore);
    resources->englishDefinitionDictPath =
        englishDefinitionDictionaryPath(paths.englishDefinitionDictionary);

    auto dictionary = std::make_unique<libime::PinyinDictionary>();
    if (!paths.dictionary.empty()) {
        if (isRegularFile(paths.dictionary)) {
            dictionary->load(0, paths.dictionary.c_str(),
                             libime::PinyinDictFormat::Binary);
        } else {
            std::cerr << "ModernIME 警告: 拼音系统词典文件不存在: '"
                      << paths.dictionary
                      << "'，拼音基础候选将无法生成！请检查 libime 安装或 PinyinDataPaths 配置。"
                      << std::endl;
        }
    }
    resources->userDictionary.addTo(*dictionary, 1);
    loadExtensionDictionary(
        *dictionary,
        extensionDictionaryPath(paths.extensionDictionary));
    // 热词表加载在知识库之后的下一层（当前为 layer 3）；正 cost 让
    // 简拼命中的热词越过系统词典组合，排在候选列表前列。
    loadExtensionDictionary(
        *dictionary, hotwordDictionaryPath(paths.hotwordDictionary));

    std::unique_ptr<libime::UserLanguageModel> model;
    if (!paths.languageModel.empty() && isRegularFile(paths.languageModel)) {
        model = std::make_unique<libime::UserLanguageModel>(
            paths.languageModel.c_str());
    } else {
        if (!paths.languageModel.empty()) {
            std::cerr << "ModernIME 警告: 拼音语言模型文件不存在: '"
                      << paths.languageModel
                      << "'，已降级为空语言模型，整句预测与候选词排序将受严重影响！请检查 libime 安装或 PinyinDataPaths 配置。"
                      << std::endl;
        }
        model = std::make_unique<libime::UserLanguageModel>();
    }

    resources->ime = std::make_unique<libime::PinyinIME>(std::move(dictionary),
                                                         std::move(model));
    resources->ime->setFuzzyFlags(getDefaultFuzzyFlags());
    resources->ime->setNBest(32);
    // 在后台预热线程中一体化加载学习写入器，首键打字时直接读取内存快照，
    // 避免首键触发数十毫秒的 SQLite 磁盘打开与全表反序列化。
    if (options.learningEnabled) {
        resources->ensureLearningWriter();
    }
    return resources;
}

bool PinyinCandidateProvider::reloadUserDictionary(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return false;
    }
    return resources->reloadUserDictionary();
}

bool PinyinCandidateProvider::reloadLearningStore(
    std::shared_ptr<SharedResources> &resources) {
    if (resources == nullptr) {
        return false;
    }
    return resources->reloadLearningStore();
}

bool PinyinCandidateProvider::refreshLearningStore(
    std::shared_ptr<SharedResources> &resources) {
    if (resources != nullptr && resources->learning != nullptr) {
        if (resources->learning->reloadIfChanged()) {
            if (resources->phraseStats != nullptr) {
                resources->phraseStats->clear();
            }
            return true;
        }
    }
    return false;
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
          clipboardTrigger_(options.clipboardTrigger) {
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
        }
    }

    bool select(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        const auto &candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::EnglishDefinition ||
            candidate.source == core::CandidateSource::Command) {
            phraseTracker_.cancelComposition();
            phraseTracker_.resetCommitHistory();
            context->clear();
            refresh();
            return true;
        }

        const auto rawInput = context->userInput();
        const bool partialSelection = candidate.consumedInputBytes > 0 &&
                                      candidate.consumedInputBytes < rawInput.size();

        core::CommitEvent event;
        event.text = candidate.text;
        event.isPartial = partialSelection;
        event.consumedInputBytes = candidate.consumedInputBytes > 0 ? candidate.consumedInputBytes : rawInput.size();
        if (candidate.fullPinyin.empty()) {
            event.fullPinyin = partialSelection
                ? std::string(rawInput.substr(0, event.consumedInputBytes))
                : std::string(rawInput);
        } else {
            event.fullPinyin = candidate.fullPinyin;
        }
        event.rawKeystrokes = std::string(rawInput.substr(0, event.consumedInputBytes));
        event.contextBefore = contextBefore_;
        event.contextAfter = contextAfter_;
        event.timestampMs = nowMilliseconds();
        if (candidate.source == core::CandidateSource::UserDictionary) {
            event.role = core::CommitCandidateRole::UserDictionary;
        } else if (candidate.source == core::CandidateSource::Raw) {
            event.role = core::CommitCandidateRole::Raw;
        } else if (core::EnglishDictionary::isEnglishWord(candidate.text)) {
            event.role = core::CommitCandidateRole::English;
        } else {
            event.role = core::CommitCandidateRole::Chinese;
        }

        if (partialSelection) {
            phraseTracker_.recordPartialSelection(
                candidate.text, event.fullPinyin, rawInput, event.consumedInputBytes);
            onCommitEvent(event);
        } else {
            auto formedIntra = phraseTracker_.recordFinalSelection(
                candidate.text, event.fullPinyin, rawInput, event.consumedInputBytes);
            onCommitEvent(event);
            if (formedIntra.has_value()) {
                core::CommitEvent phraseEvent;
                phraseEvent.text = formedIntra->text;
                phraseEvent.fullPinyin = formedIntra->fullPinyin;
                phraseEvent.rawKeystrokes = formedIntra->rawKeystrokes;
                phraseEvent.role = core::CommitCandidateRole::Chinese;
                phraseEvent.isPartial = false;
                phraseEvent.consumedInputBytes = formedIntra->rawKeystrokes.size();
                phraseEvent.timestampMs = event.timestampMs;
                onCommitEvent(phraseEvent);
                phraseTracker_.resetCommitHistory();
            } else {
                auto formedInter = phraseTracker_.recordCommit(event);
                for (const auto &fp : formedInter) {
                    core::CommitEvent phraseEvent;
                    phraseEvent.text = fp.text;
                    phraseEvent.fullPinyin = fp.fullPinyin;
                    phraseEvent.rawKeystrokes = fp.rawKeystrokes;
                    phraseEvent.role = core::CommitCandidateRole::Chinese;
                    phraseEvent.isPartial = false;
                    phraseEvent.consumedInputBytes = fp.rawKeystrokes.size();
                    phraseEvent.timestampMs = event.timestampMs;
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
            return true;
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
                return false;
            }
            refresh();
            return true;
        }
        if (candidate.source == core::CandidateSource::Learned ||
            candidate.source == core::CandidateSource::UserDictionary ||
            !candidate.consumedInputBytes ||
            candidate.consumedInputBytes >= rawInput.size()) {
            context->clear();
            refresh();
            return true;
        }
        try {
            if (page_.items[index].sourceIndex < context->candidates().size()) {
                context->select(page_.items[index].sourceIndex);
            } else {
                context->clear();
            }
        } catch (const std::exception &) {
            context->clear();
        }
        refresh();
        return true;
    }

    bool extractCharacter(std::size_t index, bool firstChar,
                          std::string *committedText) {
        if (index >= page_.items.size()) {
            return false;
        }
        const auto &candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::Command ||
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
        core::CommitEvent event;
        event.text = chosenChar;
        event.fullPinyin = chosenSyllable;
        event.rawKeystrokes = chosenSyllable;
        event.role = core::CommitCandidateRole::Chinese;
        event.isPartial = false;
        event.consumedInputBytes = rawInput.size();
        event.contextBefore = contextBefore_;
        event.contextAfter = contextAfter_;
        event.timestampMs = nowMilliseconds();

        phraseTracker_.cancelComposition();
        onCommitEvent(event);
        appendFallbackContext(chosenChar);

        context->clear();
        refresh();
        return true;
    }

    bool remove(std::size_t index) {
        if (index >= page_.items.size()) {
            return false;
        }
        const auto candidate = page_.items[index];
        if (candidate.source == core::CandidateSource::EnglishDefinition ||
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
            // 插入会话级屏蔽集合，让 refresh() 把它过滤出候选列表，
            // 否则用户按删词后系统词典词原地复活，删词形同虚设。
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
        if (text.empty() || !contextLearningEnabled_) {
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
        phraseTracker_.resetCommitHistory();
        fallbackContextBefore_.clear();
        contextBefore_.clear();
        contextAfter_.clear();
    }

    void setContext(std::string_view before, std::string_view after) {
        if (!contextLearningEnabled_) {
            contextBefore_.clear();
            contextAfter_.clear();
            fallbackContextBefore_.clear();
            return;
        }
        contextBefore_ = before;
        contextAfter_ = after;
        fallbackContextBefore_ = before;
    }

    void setLearningEnabled(bool enabled) { learningEnabled_ = enabled; }

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

    const core::CandidatePage &page() const { return page_; }

private:
    libime::PinyinIME &ime() { return *shared_->ime; }
    core::LearningWriter *learningWriter() {
        if (!learningEnabled_) {
            return nullptr;
        }
        return shared_->ensureLearningWriter();
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
            return;
        }

        const auto learning = [&] {
            auto *writer = learningWriter();
            return writer != nullptr ? writer->snapshot() : nullptr;
        }();
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
        const auto result = buildCandidatePipeline(
            *context, *ime().dict(), learning.get(), nowMilliseconds(),
            contextBefore_, contextAfter_, previousOrder, typoContext.get());
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
            if (!isManual && suppressedLearned_.contains(key)) {
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
            item.source = isManual
                              ? core::CandidateSource::UserDictionary
                              : (isLearned ? core::CandidateSource::Learned
                                           : core::CandidateSource::Engine);
            if (isManual) {
                ++manualCount;
            }
            item.consumedInputBytes = rawInput.size();
            if (core::PinyinMatchPolicy::priority(
                    rawInput, candidate.full_pinyin) == -1) {
                const auto candidatePinyin =
                    core::PinyinMatchPolicy::canonical(candidate.full_pinyin);
                for (const auto &[prefixEnd, canonicalPrefix] : canonicalPrefixes) {
                    if (canonicalPrefix == candidatePinyin) {
                        item.consumedInputBytes = prefixEnd;
                        break;
                    }
                }
            }
            if (item.consumedInputBytes < rawInput.size()) {
                partialPool.push_back(std::move(item));
            } else {
                fullItems.push_back(std::move(item));
            }
        }

        interleaveHigherNativeRankFuzzyCandidate(rawInput, fullItems);
        const auto *bestFullSentence =
            fullItems.empty() ? nullptr : &fullItems.front();
        page_.items = mixCandidateItems(fullItems, partialPool,
                                        bestFullSentence, prefixEnds, rawInput);

        const auto macroCandidates =
            generateMacroCandidates(rawInput, canonicalInput);
        if (!macroCandidates.empty()) {
            std::size_t insertPos = (page_.items.empty() || rawInput.front() == 'v') ? 0 : 1;
            for (const auto &macroText : macroCandidates) {
                if (std::any_of(page_.items.begin(), page_.items.end(),
                                [&](const auto &existing) { return existing.text == macroText; })) {
                    continue;
                }
                core::CandidateItem item;
                item.text = macroText;
                item.fullPinyin = std::string(rawInput);
                item.source = core::CandidateSource::Engine;
                item.sourceIndex = std::numeric_limits<std::size_t>::max();
                item.consumedInputBytes = rawInput.size();
                if (insertPos < page_.items.size()) {
                    page_.items.insert(page_.items.begin() + insertPos, std::move(item));
                } else {
                    page_.items.push_back(std::move(item));
                }
                ++insertPos;
            }
        }

        const bool isInitialConsonant = (rawInput == "sh" || rawInput == "ch" || rawInput == "zh");
        const bool isConsonantAbbreviation =
            rawInput.size() >= 2 && !page_.items.empty() &&
            std::all_of(rawInput.begin(), rawInput.end(), [](char c) {
                return c >= 'a' && c <= 'z' &&
                       c != 'a' && c != 'e' && c != 'i' && c != 'o' && c != 'u' && c != 'v' && c != 'y';
            }) &&
            (rawInput.size() == 2 ||
             core::PinyinMatchPolicy::abbreviationKey(
                 page_.items.front().fullPinyin) == rawInput ||
             core::PinyinMatchPolicy::initialsMatch(
                 rawInput, page_.items.front().fullPinyin));
        const bool isEnglish =
            !isInitialConsonant && !isConsonantAbbreviation &&
            core::EnglishDictionary::isEnglishWord(rawInput);

        const bool hasExactFullPinyinMatch = std::any_of(
            page_.items.begin(), page_.items.end(),
            [&rawInput](const auto &item) {
                return item.source != core::CandidateSource::Raw &&
                       (core::PinyinMatchPolicy::exactInputMatch(
                            rawInput, item.fullPinyin) ||
                        core::PinyinMatchPolicy::isFullTypoMatch(
                            rawInput, item.fullPinyin));
            });

        const bool hasTrustedShortAbbreviation =
            std::any_of(page_.items.begin(), page_.items.end(),
                        [&rawInput](const auto &item) {
                            return item.source != core::CandidateSource::Raw &&
                                   core::PinyinMatchPolicy::
                                       trustedShortAbbreviationMatch(
                                           rawInput, item.fullPinyin,
                                           item.text);
                        });

        bool isTopChineseLexical = false;
        if (!page_.items.empty()) {
            const auto &topChinese = page_.items.front();
            if (topChinese.source == core::CandidateSource::UserDictionary) {
                isTopChineseLexical = true;
            } else if (topChinese.sourceIndex < result.scored.size() &&
                       (result.scored[topChinese.sourceIndex].dictionary_bonus > 0.0 ||
                        result.scored[topChinese.sourceIndex].is_lexical)) {
                isTopChineseLexical = true;
            } else if (utf8CodePointCount(topChinese.text) == 1 &&
                       rawInput.size() <= 3) {
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
            !page_.items.empty() &&
            (hasTrustedShortAbbreviation || isConsonantAbbreviation ||
             (hasExactFullPinyinMatch && isTopChineseLexical));

        const bool shouldPredictEnglish =
            (rawInput.empty() || rawInput.front() != 'v') &&
            !isEnglish && !hasHighConfidenceChinese &&
            !hasExactFullPinyinMatch &&
            (!hasPinyinCoverage ||
             hasInvalidInternalPinyinSegment(segmentedInput)) &&
            rawInput.size() >= 4;

        std::vector<std::string> predictedWords;
        const bool canPredictEnglish =
            (rawInput.empty() || rawInput.front() != 'v') &&
            !hasTrustedShortAbbreviation &&
            !isConsonantAbbreviation &&
            rawInput.size() >= 2;

        if (canPredictEnglish && learning != nullptr) {
            const auto learned = learning->recallEnglishWords(rawInput, 3, nowMilliseconds());
            for (const auto &item : learned) {
                if (!suppressedLearned_.contains(candidateKey(item.text, item.text)) &&
                    !learning->isSuppressed(item.text, item.text)) {
                    predictedWords.push_back(item.text);
                }
            }
        }
        if (shouldPredictEnglish) {
            const auto staticWords = core::EnglishDictionary::predictWords(rawInput, 3);
            for (const auto &w : staticWords) {
                if (std::find(predictedWords.begin(), predictedWords.end(), w) == predictedWords.end()) {
                    predictedWords.emplace_back(w);
                }
            }
        }

        const bool isEnglishSuppressed =
            suppressedLearned_.contains(candidateKey(rawInput, rawInput)) ||
            (learning != nullptr && learning->isSuppressed(rawInput, rawInput));

        bool preferEnglishOverChinese = false;
        if (page_.items.empty()) {
            preferEnglishOverChinese = true;
        } else if (hasHighConfidenceChinese) {
            preferEnglishOverChinese = false;
        } else if (!predictedWords.empty() && !hasExactFullPinyinMatch) {
            preferEnglishOverChinese = true;
        } else if (isEnglish && !isEnglishSuppressed) {
            if (!hasExactFullPinyinMatch) {
                preferEnglishOverChinese = true;
            } else if (!isTopChineseLexical) {
                const double chineseLearningBoost =
                    (page_.items.front().sourceIndex < result.scored.size())
                        ? result.scored[page_.items.front().sourceIndex].learning_boost
                        : 0.0;
                if (chineseLearningBoost < 2.5) {
                    preferEnglishOverChinese = true;
                }
            }
        }

        core::CandidateItem rawCandidate;
        rawCandidate.text = rawInput;
        rawCandidate.fullPinyin = std::string(rawInput);
        rawCandidate.source = core::CandidateSource::Raw;
        rawCandidate.consumedInputBytes = rawInput.size();

        page_.items.erase(
            std::remove_if(page_.items.begin(), page_.items.end(),
                           [](const auto &item) {
                                return item.source == core::CandidateSource::Raw;
                           }),
            page_.items.end());

        char triggerChar = 'v';
        if (clipboardTrigger_.size() == 3 && clipboardTrigger_[1] == '+') {
            triggerChar = static_cast<char>(std::tolower(static_cast<unsigned char>(clipboardTrigger_[0])));
        }
        const bool isSingleTriggerChar =
            rawInput.size() == 1 &&
            std::tolower(static_cast<unsigned char>(rawInput.front())) == triggerChar;

        if (isSingleTriggerChar && rawInput == "v") {
            page_.items.clear();
            core::CandidateItem rawItem;
            rawItem.text = "v";
            rawItem.fullPinyin = "v";
            rawItem.source = core::CandidateSource::Raw;
            rawItem.consumedInputBytes = 1;
            page_.items.push_back(std::move(rawItem));

            if (clipboardEnabled_) {
                core::CandidateItem cmdCandidate;
                cmdCandidate.text = "剪贴板 (" + clipboardTrigger_ + ")";
                cmdCandidate.fullPinyin = "v";
                cmdCandidate.source = core::CandidateSource::Command;
                cmdCandidate.consumedInputBytes = 1;
                page_.items.push_back(std::move(cmdCandidate));
            }
            page_.preedit = "v";
        } else if (!macroCandidates.empty() && rawInput.front() == 'v') {
            page_.items.push_back(std::move(rawCandidate));
            page_.preedit = std::string(rawInput);
        } else if (preferEnglishOverChinese) {
            if (!predictedWords.empty()) {
                core::CandidateItem predictedCandidate;
                predictedCandidate.text = std::string(predictedWords.front());
                predictedCandidate.fullPinyin = std::string(rawInput);
                predictedCandidate.source = core::CandidateSource::Raw;
                predictedCandidate.consumedInputBytes = rawInput.size();

                page_.items.insert(page_.items.begin(), std::move(rawCandidate));
                page_.items.insert(page_.items.begin(), std::move(predictedCandidate));
            } else {
                page_.items.insert(page_.items.begin(), std::move(rawCandidate));
            }
            page_.preedit = std::string(rawInput);
        } else {
            if (!predictedWords.empty()) {
                core::CandidateItem predictedCandidate;
                predictedCandidate.text = std::string(predictedWords.front());
                predictedCandidate.fullPinyin = std::string(rawInput);
                predictedCandidate.source = core::CandidateSource::Raw;
                predictedCandidate.consumedInputBytes = rawInput.size();

                const auto insertPos =
                    page_.items.empty() ? page_.items.begin() : std::next(page_.items.begin());
                page_.items.insert(insertPos, std::move(predictedCandidate));
            }

            if (isEnglish || !predictedWords.empty()) {
                bool isSingleSyllable = false;
                if (!page_.items.empty()) {
                    isSingleSyllable = pinyinSyllableCount(page_.items.front().fullPinyin) <= 1;
                }
                const bool isDualAttributeExemption = (rawInput == "can");
                if (isSingleSyllable && !isDualAttributeExemption) {
                    page_.items.push_back(std::move(rawCandidate));
                } else {
                    const auto insertPos =
                        page_.items.empty() ? page_.items.begin() : std::next(page_.items.begin());
                    page_.items.insert(insertPos, std::move(rawCandidate));
                }
            } else {
                page_.items.push_back(std::move(rawCandidate));
            }
            page_.preedit = segmentedInput;
        }

        if (englishDefinitionEnabled_ && !page_.items.empty() && shared_) {
            auto &top = page_.items.front();
            if (top.source == core::CandidateSource::Raw &&
                top.text.size() >= 4 &&
                core::EnglishDictionary::isEnglishWord(top.text)) {
                auto *dict = shared_->ensureEnglishDefinitionDictionary();
                if (dict != nullptr) {
                    const auto rawDef = dict->lookup(top.text);
                    if (!rawDef.empty()) {
                        auto cleaned = core::EnglishDefinitionDictionary::cleanDefinition(rawDef, 12);
                        if (!cleaned.empty()) {
                            top.annotation = std::move(cleaned);
                        }
                    }
                }
            }
        }

        std::unordered_set<std::string> seenCandidateTexts;
        page_.items.erase(
            std::remove_if(page_.items.begin(), page_.items.end(),
                           [&seenCandidateTexts](const auto &item) {
                               if (item.source == core::CandidateSource::Command) {
                                   return false;
                               }
                               return !seenCandidateTexts.insert(item.text).second;
                           }),
            page_.items.end());
    }

    std::shared_ptr<SharedResources> shared_;
    core::PhraseFormationTracker phraseTracker_;
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

} // namespace modernime::pinyin
