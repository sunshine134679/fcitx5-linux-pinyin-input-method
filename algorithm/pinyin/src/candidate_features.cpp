#include "modernime/pinyin/candidate_features.h"
#include "modernime/pinyin/recent_choice.h"
#include "modernime/core/common_characters.h"
#include "modernime/core/dictionary_prior.h"
#include "modernime/core/pinyin_match.h"
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>
#include <algorithm>
namespace modernime::pinyin {
namespace {
bool validFullPinyin(std::string_view fullPinyin) {
    if (fullPinyin.empty() || fullPinyin.front() == '\'' ||
        fullPinyin.back() == '\'') {
        return false;
    }
    bool separator = false;
    for (const char character : fullPinyin) {
        if (character == '\'') {
            if (separator) {
                return false;
            }
            separator = true;
        } else if (character >= 'a' && character <= 'z') {
            separator = false;
        } else {
            return false;
        }
    }
    return true;
}

std::size_t utf8CodePointCount(std::string_view text) {
    std::size_t count = 0;
    for (const char character : text) {
        if ((static_cast<unsigned char>(character) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

struct DictBonusResult {
    double bonus = 0.0;
    bool is_lexical = false;
};

DictBonusResult dictionaryBonus(const libime::PinyinDictionary &dictionary,
                               std::string_view fullPinyin,
                               std::string_view phrase) {
    if (!validFullPinyin(fullPinyin)) {
        return {0.0, false};
    }
    // 单字不加系统词典加分：libime 语言模型对单字 unigram 的原生排序
    // 更可靠（如 de→的、a→啊），sc.dict 的离散权重（0=中性、负=加权）
    // 反而会把高频单字顶离首位。用户词典层（cost>=0）不受此限制，
    // 用户手动收录的单字仍然可以上位。
    const std::size_t phraseLength = utf8CodePointCount(phrase);
    const bool singleCharacter = phraseLength == 1;
    std::vector<char> encoded;
    try {
        encoded = libime::PinyinEncoder::encodeFullPinyin(fullPinyin);
    } catch (const std::exception &) {
        return {0.0, false};
    }
    double userBonus = 0.0;
    double systemBonus = 0.0;
    bool isLexical = false;
    dictionary.matchWords(
        encoded.data(), encoded.size(),
        [&userBonus, &systemBonus, &isLexical, phrase,
         singleCharacter, phraseLength](std::string_view,
                          std::string_view hanzi,
                          float cost) {
            if (hanzi != phrase) {
                return true;
            }
            isLexical = true;
            if (cost >= 0.0F) {
                if (!singleCharacter || cost > 0.0F) {
                    userBonus = std::max(userBonus,
                                         core::curatedDictionaryBonus(cost, phraseLength));
                }
            } else if (!singleCharacter) {
                systemBonus = std::max(systemBonus,
                                       core::systemDictionaryBonus(cost));
            }
            // 同一 phrase 在用户词典与系统词典中可能各出现一次，
            // 两种来源都命中后即可提前终止，无需继续遍历剩余前缀子树。
            return userBonus <= 0.0 || systemBonus <= 0.0;
        });
    return {core::combinedDictionaryBonus(userBonus, systemBonus), isLexical};
}

} // namespace
void populateCandidateFeatures(core::CandidateScore &candidate,
                               const libime::PinyinDictionary &dictionary,
                               const CandidateFeatureContext &request) {
    const auto dict = dictionaryBonus(dictionary, candidate.full_pinyin, candidate.text);
    candidate.dictionary_bonus = dict.bonus;
    const auto count = utf8CodePointCount(candidate.text);
    candidate.is_lexical = count >= 2 && dict.is_lexical;
    candidate.is_rare = core::CommonCharacters::containsRare(candidate.text);
    candidate.is_level1 = !candidate.is_rare && count == 1 &&
        core::CommonCharacters::isLevel1(core::CommonCharacters::firstCodepoint(candidate.text));
    if (request.learning != nullptr) {
        // The snapshot owns signed evidence. Zero means no history, not a
        // lower bound: max(default_zero, boost) would erase negative feedback.
        candidate.learning_boost = request.learning->boostAt(
            candidate.text, candidate.full_pinyin, request.nowMs);
        candidate.context_bonus = request.learning->contextBoost(
            candidate.text, candidate.full_pinyin, request.before, request.after);
        candidate.last_selected_ms = request.learning->lastSelectedMs(
            candidate.text, candidate.full_pinyin);
    }
    if (count != 1 || candidate.learning_boost >= 1.8) candidate.unigram_anchor = 0.0;
    candidate.recent_choice_bonus = 0.0;
    if (request.recentChoices != nullptr && candidate.learning_boost >= 1.8) {
        candidate.recent_choice_bonus = request.recentChoices->bonus(
            request.input, candidate.text, candidate.full_pinyin,
            core::PinyinMatchPolicy::priority(request.input, candidate.full_pinyin) == -1);
    }
}
} // namespace modernime::pinyin
