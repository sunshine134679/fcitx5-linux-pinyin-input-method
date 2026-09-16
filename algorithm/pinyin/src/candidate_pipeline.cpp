#include "modernime/pinyin/candidate_pipeline.h"

#include "modernime/core/dictionary_prior.h"

#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>

#include <algorithm>
#include <string_view>

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

double dictionaryBonus(const libime::PinyinDictionary &dictionary,
                       std::string_view fullPinyin,
                       std::string_view phrase) {
    if (!validFullPinyin(fullPinyin)) {
        return 0.0;
    }
    // 单字不加系统词典加分：libime 语言模型对单字 unigram 的原生排序
    // 更可靠（如 de→的、a→啊），sc.dict 的离散权重（0=中性、负=加权）
    // 反而会把高频单字顶离首位。用户词典层（cost>=0）不受此限制，
    // 用户手动收录的单字仍然可以上位。
    const bool singleCharacter = utf8CodePointCount(phrase) == 1;
    const auto encoded = libime::PinyinEncoder::encodeFullPinyin(fullPinyin);
    double userBonus = 0.0;
    double systemBonus = 0.0;
    dictionary.matchWords(
        encoded.data(), encoded.size(),
        [&userBonus, &systemBonus, phrase,
         singleCharacter](std::string_view,
                          std::string_view hanzi,
                          float cost) {
            if (hanzi != phrase) {
                return true;
            }
            if (cost >= 0.0F) {
                userBonus = std::max(userBonus,
                                     core::curatedDictionaryBonus(cost));
            } else if (!singleCharacter) {
                systemBonus = std::max(systemBonus,
                                       core::systemDictionaryBonus(cost));
            }
            // 同一 phrase 在用户词典与系统词典中可能各出现一次，
            // 两种来源都命中后即可提前终止，无需继续遍历剩余前缀子树。
            return userBonus <= 0.0 || systemBonus <= 0.0;
        });
    return core::combinedDictionaryBonus(userBonus, systemBonus);
}

} // namespace

CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter,
    const std::vector<std::string> &previousOrder) {
    CandidatePipelineResult result;
    const auto &nativeCandidates = context.candidates();
    // libime 的候选已按解码代价升序排列。简拼输入（如 yyds）会展开出
    // 数千个候选，全部进入学习 boost + 词典加分评分循环会让单键耗时
    // 超过 100ms；用户翻页永远看不到 128 名以外的候选，这里按 libime
    // 原生顺序截断头部再评分，保证评分管线只在有限集合上工作。
    constexpr std::size_t kMaxScoredCandidates = 128;
    const std::size_t scoreLimit =
        nativeCandidates.size() < kMaxScoredCandidates
            ? nativeCandidates.size()
            : kMaxScoredCandidates;
    result.scored.reserve(scoreLimit);
    for (std::size_t index = 0; index < scoreLimit; ++index) {
        core::CandidateScore candidate;
        candidate.source_index = index;
        candidate.text = nativeCandidates[index].toString();
        candidate.full_pinyin = context.candidateFullPinyin(index);
        candidate.decoder_score = nativeCandidates[index].score();
        candidate.dictionary_bonus = dictionaryBonus(
            dictionary, candidate.full_pinyin, candidate.text);
        if (learning != nullptr) {
            candidate.learning_boost = learning->boostAt(
                candidate.text, candidate.full_pinyin, nowMs);
            candidate.context_bonus = learning->contextBoost(
                candidate.text, candidate.full_pinyin, contextBefore,
                contextAfter);
        }
        result.scored.push_back(std::move(candidate));
    }
    result.order = core::CandidateRanker::rank(
        context.userInput(), result.scored, previousOrder);
    return result;
}

} // namespace modernime::pinyin
