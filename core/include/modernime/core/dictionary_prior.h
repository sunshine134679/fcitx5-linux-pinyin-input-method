#pragma once

#include <algorithm>
#include <cmath>

namespace modernime::core {

// LibIME's user-dictionary cost is an insertion-cost hint rather than a
// calibrated probability. Keep it bounded so native decoder order remains
// the primary signal.
inline double curatedDictionaryBonus(float cost) {
    if (cost <= 0.0F) {
        return 0.0;
    }
    constexpr double maximumCost = 100.0;
    constexpr double maximumBonus = 1.5;
    return maximumBonus * std::log1p(static_cast<double>(cost)) /
           std::log1p(maximumCost);
}

inline double systemDictionaryBonus(float cost) {
    return std::clamp(-static_cast<double>(cost), 0.0, 4.0);
}

inline double combinedDictionaryBonus(double userBonus, double systemBonus) {
    return std::min(12.0, userBonus + systemBonus);
}

} // namespace modernime::core
