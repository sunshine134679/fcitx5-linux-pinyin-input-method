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

double dictionaryBonus(const libime::PinyinDictionary &dictionary,
                       std::string_view fullPinyin,
                       std::string_view phrase) {
    if (!validFullPinyin(fullPinyin)) {
        return 0.0;
    }
    const auto encoded = libime::PinyinEncoder::encodeFullPinyin(fullPinyin);
    double userBonus = 0.0;
    double systemBonus = 0.0;
    dictionary.matchWords(
        encoded.data(), encoded.size(),
        [&userBonus, &systemBonus, phrase](std::string_view,
                                            std::string_view hanzi,
                                            float cost) {
            if (hanzi != phrase) {
                return true;
            }
            if (cost >= 0.0F) {
                userBonus = std::max(userBonus,
                                     core::curatedDictionaryBonus(cost));
            } else {
                systemBonus = std::max(systemBonus,
                                       core::systemDictionaryBonus(cost));
            }
            return true;
        });
    return core::combinedDictionaryBonus(userBonus, systemBonus);
}

} // namespace

CandidatePipelineResult buildCandidatePipeline(
    const libime::PinyinContext &context,
    const libime::PinyinDictionary &dictionary,
    const core::LearningSnapshot *learning, std::int64_t nowMs,
    std::string_view contextBefore, std::string_view contextAfter) {
    CandidatePipelineResult result;
    const auto &nativeCandidates = context.candidates();
    result.scored.reserve(nativeCandidates.size());
    for (std::size_t index = 0; index < nativeCandidates.size(); ++index) {
        core::CandidateScore candidate;
        candidate.source_index = index;
        candidate.text = nativeCandidates[index].toString();
        candidate.full_pinyin = context.candidateFullPinyin(index);
        candidate.decoder_score = nativeCandidates[index].score();
        candidate.dictionary_bonus = dictionaryBonus(
            dictionary, candidate.full_pinyin, candidate.text);
        if (learning != nullptr) {
            candidate.learning_boost = learning->boostAt(
                candidate.text, candidate.full_pinyin, contextBefore,
                contextAfter, nowMs);
            candidate.context_bonus = learning->contextBoost(
                candidate.text, candidate.full_pinyin, contextBefore,
                contextAfter);
        }
        result.scored.push_back(std::move(candidate));
    }
    result.order = core::CandidateRanker::rank(
        context.userInput(), result.scored);
    return result;
}

} // namespace modernime::pinyin
