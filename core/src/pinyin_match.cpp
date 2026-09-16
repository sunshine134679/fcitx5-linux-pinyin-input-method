#include "modernime/core/pinyin_match.h"

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

int PinyinMatchPolicy::priority(std::string_view userInput,
                                std::string_view fullPinyin) {
    // 原实现经 exactInputMatch 兜底各调一次 canonical（每候选四次字符串
    // 分配）；ranker 对每个候选调用本函数，这里直接算一次复用。
    const auto input = canonical(userInput);
    const auto candidate = canonical(fullPinyin);
    if (candidate == input) {
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
