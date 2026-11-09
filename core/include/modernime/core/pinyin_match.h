#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

class PinyinMatchPolicy final {
public:
    static std::string canonical(std::string_view fullPinyin);
    static std::string segmentPinyin(std::string_view fullPinyin);
    static std::string abbreviationKey(std::string_view fullPinyin);
    static bool initialsMatch(std::string_view userInput, std::string_view fullPinyin);
    // 按候选音节对齐全音节、单字母声母或 zh/ch/sh，要求完整覆盖。
    // 不把用户输入的单一贪心切分当作候选音节边界。
    static bool syllableInputMatch(std::string_view userInput, std::string_view fullPinyin);
    // 查询端有界探索：最多 512 个状态、64 个声母键，单键不写回学习库。
    // hasIndexedPrefix 在学习召回中按现有索引剪枝，假想键不消耗结果预算。
    static std::vector<std::string> mixedAbbreviationKeys(
        std::string_view userInput,
        const std::function<bool(std::string_view)> &hasIndexedPrefix = {});
    static bool validComposition(std::string_view userInput);
    static bool isAbbreviationInput(std::string_view userInput);
    static bool exactInputMatch(std::string_view userInput,
                                std::string_view fullPinyin);
    static bool trustedShortAbbreviationMatch(std::string_view userInput,
                                              std::string_view fullPinyin,
                                              std::string_view text);
    static std::string normalizeTypoInput(std::string_view input);
    static std::size_t matchTypoSyllable(std::string_view input,
                                         std::string_view syllable);
    static bool isTypoPrefix(std::string_view input,
                             std::string_view syllable);
    static bool isFullTypoMatch(std::string_view userInput,
                                std::string_view fullPinyin);
    // 已召回候选的六组标准模糊音等价键；不含 u/e 等额外近音纠错。
    // 只用于排序资格，是否启用各组仍由解码配置负责。
    static std::string phoneticFuzzyKey(std::string_view fullPinyin);
    static int priority(std::string_view userInput,
                        std::string_view fullPinyin);
    static bool isValidPinyinSequence(std::string_view text);
    // 单串是否为标准普通话音节（kValidPinyinSyllables 表内成员）
    static bool isStandardSyllable(std::string_view syllable);
    // 容错变体输入（2026-10-24 容错专项）：对有效拼音串构造"近音变体"——
    // 每次至多修改一个音节（u↔e 韵尾、ui↔iu、多打 g 去除、介音 i 补全、
    // 无效单音节的 r↔l 声母互换），仅在替换结果仍是合法音节时采纳。
    // 返回原串表示无可行变体。变体只作为低位补充召回来源，绝不改写主解码。
    static std::string rhymeVariantInput(std::string_view input);
    static bool isCoreChineseWord(std::string_view text, std::string_view pinyin);
};

} // namespace modernime::core
