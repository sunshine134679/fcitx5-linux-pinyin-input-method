#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace modernime::core {

// Derived from immutable learning facts. Time remains a query input so cached
// evidence never freezes the existing 30-day forgetting curve.
struct LearningBoostEvidence {
    std::int64_t frequency = 0;
    std::int64_t lastSelectedMs = 0;
    double frequencyPart = 0.0;
    double recentScale = 0.0;
    double penalty = 0.0;
    bool suppressed = false;
};

inline LearningBoostEvidence makeLearningBoostEvidence(
    std::int64_t frequency, std::int64_t genericFrequency,
    std::size_t distinctContexts, std::int64_t lastSelectedMs,
    std::int64_t worstFeedback, bool suppressed) {
    double effective = static_cast<double>(frequency);
    if (genericFrequency == 0 && distinctContexts < 3)
        effective = std::max(0.0, 0.25 * effective);
    const double penalty = worstFeedback > 0
        ? 1.05 + 0.85 * std::log1p(static_cast<double>(worstFeedback - 1))
        : 0.0;
    return {frequency, lastSelectedMs,
            std::min(3.0, 0.85 * std::log1p(effective)),
            effective >= 3.0 ? 0.90 : (effective >= 2.0 ? 0.60 : (effective >= 1.0 ? 0.35 : 0.15)),
            penalty,
            suppressed};
}

inline double learningBoostAt(const LearningBoostEvidence &evidence, std::int64_t nowMs) {
    if (evidence.suppressed || evidence.frequency <= 0) return 0.0;
    constexpr double tauMs = (30.0 * 24.0 * 60.0 * 60.0 * 1000.0) / 0.6931471805599453;
    constexpr double kDayMs = 24.0 * 60.0 * 60.0 * 1000.0;
    const double age = nowMs > 0
        ? std::max(0.0, static_cast<double>(nowMs) - static_cast<double>(evidence.lastSelectedMs))
        : 0.0;
    double freqPart = evidence.frequencyPart;
    double recent = evidence.recentScale;
    if (age > 0.0) {
        recent = std::min(1.0, evidence.recentScale * std::exp(-age / tauMs));
        if (evidence.frequency == 1) {
            constexpr double singletonTauMs = (14.0 * kDayMs) / 0.6931471805599453;
            const double fade = std::exp(-age / singletonTauMs);
            freqPart *= fade;
            recent *= fade;
        } else if (evidence.frequency == 2) {
            constexpr double pairTauMs = (60.0 * kDayMs) / 0.6931471805599453;
            freqPart *= std::exp(-age / pairTauMs);
        }
    } else {
        recent = std::min(1.0, recent);
    }
    const double raw = freqPart + recent - evidence.penalty;
    // Compress negative tail strictly monotonically so high negative counts keep
    // distinct ordering across frequencies without widening EnglishRecallIndex dormant spread.
    const double signedBoost = raw < 0.0 ? raw * 0.25 : raw;
    return std::clamp(signedBoost, -2.0, 4.0);
}

} // namespace modernime::core
