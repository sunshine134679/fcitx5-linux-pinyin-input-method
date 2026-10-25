#include "modernime/core/candidate_ranker.h"

#include "modernime/core/candidate_model.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace modernime::core {

std::vector<std::size_t>
CandidateRanker::rank(std::string_view userInput,
                     std::vector<CandidateScore> &candidates,
                     const std::vector<std::string> &previousOrder) {
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    for (auto &candidate : candidates) {
        candidate.match_priority =
            PinyinMatchPolicy::priority(userInput, candidate.full_pinyin);
        candidate.stability_bonus = 0.0;
        candidate.decoder_bonus = 0.0;
    }
    std::unordered_map<int, std::pair<std::int64_t, double>> latestByPriority;
    for (const auto &candidate : candidates) {
        if (candidate.last_selected_ms <= 0) {
            continue;
        }
        auto &entry = latestByPriority[candidate.match_priority];
        const double boost = std::isfinite(candidate.learning_boost)
                                 ? std::max(0.0, candidate.learning_boost)
                                 : 0.0;
        if (candidate.last_selected_ms > entry.first) {
            entry.first = candidate.last_selected_ms;
            entry.second = boost;
        } else if (candidate.last_selected_ms == entry.first) {
            entry.second = std::max(entry.second, boost);
        }
    }
    constexpr double kDormantHalfLifeMs = 12.0 * 3600.0 * 1000.0;
    constexpr double kDormantTauMs = kDormantHalfLifeMs / 0.6931471805599453;
    for (auto &candidate : candidates) {
        const auto &latestEntry = latestByPriority[candidate.match_priority];
        const auto latest = latestEntry.first;
        const auto latestBoost = latestEntry.second;
        if (latest > 0 && candidate.last_selected_ms > 0 && candidate.last_selected_ms < latest) {
            const auto ageSinceLatest = latest - candidate.last_selected_ms;
            double dormant = std::exp(-static_cast<double>(ageSinceLatest) / kDormantTauMs);
            if (latestBoost >= 1.8) {
                const double discount = 1.0 - 0.30 * std::min(1.0, (latestBoost - 1.8) / (3.9 - 1.8));
                dormant = std::min(dormant, discount);
            }
            candidate.dormant_factor = dormant;
        } else {
            candidate.dormant_factor = 1.0;
        }
    }
    float minimumDecoderScore = std::numeric_limits<float>::max();
    float maximumDecoderScore = std::numeric_limits<float>::lowest();
    for (const auto &candidate : candidates) {
        if (!std::isfinite(candidate.decoder_score)) {
            continue;
        }
        minimumDecoderScore =
            std::min(minimumDecoderScore, candidate.decoder_score);
        maximumDecoderScore =
            std::max(maximumDecoderScore, candidate.decoder_score);
    }
    if (minimumDecoderScore < maximumDecoderScore) {
        const auto range = static_cast<double>(maximumDecoderScore) -
                           static_cast<double>(minimumDecoderScore);
        for (auto &candidate : candidates) {
            if (!std::isfinite(candidate.decoder_score)) {
                continue;
            }
            candidate.decoder_bonus =
                (static_cast<double>(maximumDecoderScore) -
                 static_cast<double>(candidate.decoder_score)) /
                range;
        }
    }
    if (!previousOrder.empty() && !candidates.empty()) {
        const auto baseCost = [](const CandidateScore &candidate) {
            return candidate.final_score();
        };
        double best = baseCost(candidates.front());
        for (const auto &candidate : candidates) {
            best = std::min(best, baseCost(candidate));
        }
        std::unordered_map<std::string_view, std::size_t> previousIndex;
        previousIndex.reserve(previousOrder.size());
        for (std::size_t i = 0; i < previousOrder.size(); ++i) {
            previousIndex.emplace(previousOrder[i], i);
        }
        for (auto &candidate : candidates) {
            const auto it = previousIndex.find(
                candidateOrderKey(candidate.text, candidate.full_pinyin));
            if (it == previousIndex.end()) {
                continue;
            }
            const auto distance = baseCost(candidate) - best;
            if (distance > 0.0 && distance <= 0.5) {
                const auto position = it->second;
                candidate.stability_bonus =
                    std::min(0.40, 0.40 / static_cast<double>(position + 1));
            }
        }
    }
    // Decide abbreviation promotion once for the entire candidate set. A
    // pair-dependent learning exemption can create a cycle between a trusted
    // phrase, a learned alternative, and an unlearned native leader.
    std::vector<bool> abbreviationPromoted(candidates.size(), false);
    double bestTrustedScore = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto &candidate = candidates[index];
        if (candidate.match_priority == 1 &&
            PinyinMatchPolicy::trustedShortAbbreviationMatch(
                userInput, candidate.full_pinyin, candidate.text)) {
            abbreviationPromoted[index] = true;
            bestTrustedScore = std::min(bestTrustedScore, candidate.abbreviation_admission_score());
        }
    }
    if (std::isfinite(bestTrustedScore)) {
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index];
            if (candidate.match_priority == 1 && candidate.learning_boost >= 2.5 &&
                candidate.abbreviation_admission_score() < bestTrustedScore) {
                abbreviationPromoted[index] = true;
            }
        }
    } else {
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto &candidate = candidates[index];
            if (candidate.match_priority == 1 && candidate.learning_boost >= 0.8) {
                abbreviationPromoted[index] = true;
            }
        }
    }
    std::vector<double> preferenceCosts;
    preferenceCosts.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        // Unadmitted alternatives must not use preference normalization to
        // bypass the existing trusted-abbreviation boundary.
        const auto &candidate = candidates[i];
        preferenceCosts.push_back(std::isfinite(bestTrustedScore) &&
                candidate.match_priority == 1 && !abbreviationPromoted[i]
            ? candidate.abbreviation_admission_score() : candidate.final_score());
    }
    // F1（2026-10-24 容错专项）：LibIME 模糊解码把击键形读音候选（如
    // zongwen 输入下的 zong'wen）判为 prio=2、规范读音候选（zhong'wen）判为
    // prio=0，组间排序不受学习影响，成熟偏好永远无法越过一次声母误差。
    // 与简拼晋升同构：prio=0（全覆盖模糊变体）候选在学习增益达到既有
    // 成熟学习者阈值 1.8 后，可与 prio=2 直接匹配组按最终分数竞争。
    // 门槛收紧：候选读音必须与输入音节数一致（模糊变体天然保持音节结构），
    // 排除"王绍龙王"类分段造词产生的音节数不符学习词条越组晋升。
    std::size_t inputSyllables = 0;
    bool inputIsFullPinyin = false;
    {
        const auto segmented = PinyinMatchPolicy::segmentPinyin(userInput);
        if (!segmented.empty()) {
            inputSyllables = static_cast<std::size_t>(
                                 std::count(segmented.begin(), segmented.end(), '\'')) +
                             1;
            // 2026-10-25 发布门禁回归：简拼/混拼的“段数”不是音节数，
            // 不能作为模糊音组间晋升的音节结构依据。例如 nh 下学习到的
            // 单字「你」(ni) 会因段数相同被误抬到 trusted 简拼「你好」
            // 之上。仅当每个输入段都是标准音节（全拼）时才允许该晋升。
            inputIsFullPinyin = true;
            for (std::size_t start = 0; start < segmented.size();) {
                const auto sep = segmented.find('\'', start);
                const auto stop = sep == std::string::npos ? segmented.size() : sep;
                if (stop <= start ||
                    !PinyinMatchPolicy::isStandardSyllable(
                        std::string_view(segmented).substr(start, stop - start))) {
                    inputIsFullPinyin = false;
                    break;
                }
                if (sep == std::string::npos) break;
                start = sep + 1;
            }
        }
    }
    const auto effectivePriority = [&candidates, inputSyllables,
                                    inputIsFullPinyin](std::size_t index) {
        const auto &candidate = candidates[index];
        if (candidate.match_priority != 0 || !std::isfinite(candidate.learning_boost) ||
            candidate.learning_boost < 1.8 || inputSyllables == 0 ||
            !inputIsFullPinyin || candidate.full_pinyin.empty()) {
            return candidate.match_priority;
        }
        const auto candidateSyllables =
            static_cast<std::size_t>(
                std::count(candidate.full_pinyin.begin(), candidate.full_pinyin.end(), '\'')) +
            1;
        return candidateSyllables == inputSyllables ? 2 : candidate.match_priority;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&candidates, &abbreviationPromoted, &preferenceCosts,
                      &effectivePriority](std::size_t left, std::size_t right) -> bool {
        const auto &a = candidates[left];
        const auto &b = candidates[right];
        const auto aPriority = effectivePriority(left);
        const auto bPriority = effectivePriority(right);
        if (aPriority != bPriority) {
            return aPriority > bPriority;
        }
        if (abbreviationPromoted[left] != abbreviationPromoted[right]) {
            return abbreviationPromoted[left];
        }
        const double aCost = preferenceCosts[left];
        const double bCost = preferenceCosts[right];
        const bool aFin = std::isfinite(aCost);
        const bool bFin = std::isfinite(bCost);
        if (aFin != bFin) {
            return aFin;
        }
        if (aFin && aCost != bCost) {
            return aCost < bCost;
        }
        if (a.source_index != b.source_index) {
            return a.source_index < b.source_index;
        }
        return candidateOrderKey(a.text, a.full_pinyin) <
               candidateOrderKey(b.text, b.full_pinyin);
    });
    return order;
}

} // namespace modernime::core
