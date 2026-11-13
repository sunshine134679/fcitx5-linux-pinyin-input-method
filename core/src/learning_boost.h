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
    return {frequency, lastSelectedMs,
            std::min(3.0, 0.85 * std::log1p(effective)),
            effective >= 3.0 ? 0.90 : (effective >= 2.0 ? 0.60 : (effective >= 1.0 ? 0.35 : 0.15)),
            std::min(2.0, 0.75 * std::log1p(static_cast<double>(std::max<std::int64_t>(0, worstFeedback)))),
            suppressed};
}

inline double learningBoostAt(const LearningBoostEvidence &evidence, std::int64_t nowMs) {
    if (evidence.suppressed || evidence.frequency <= 0) return 0.0;
    constexpr double tauMs = (30.0 * 24.0 * 60.0 * 60.0 * 1000.0) / 0.6931471805599453;
    const double age = std::max(0.0, static_cast<double>(nowMs) -
                                      static_cast<double>(evidence.lastSelectedMs));
    const double recent = std::min(1.0, evidence.recentScale * std::exp(-age / tauMs));
    return std::clamp(evidence.frequencyPart + recent - evidence.penalty, -2.0, 4.0);
}

} // namespace modernime::core
