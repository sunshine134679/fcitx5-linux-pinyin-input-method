#pragma once

#include <string>
#include <string_view>

namespace modernime::core {

class PinyinMatchPolicy final {
public:
    static std::string canonical(std::string_view fullPinyin);
    static std::string segmentPinyin(std::string_view fullPinyin);
    static std::string abbreviationKey(std::string_view fullPinyin);
    static bool initialsMatch(std::string_view userInput, std::string_view fullPinyin);
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
