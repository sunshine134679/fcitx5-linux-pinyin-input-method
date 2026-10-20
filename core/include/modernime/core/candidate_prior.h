#pragma once

#include <cmath>

namespace modernime::core {

// Monotone: cold-start soft evidence keeps its order, but its total range
// cannot permanently outweigh a repeatedly confirmed personal preference.
inline double boundedCandidatePrior(double rawPrior) {
    constexpr double scale = 48.0;
    return scale * rawPrior / (scale + std::abs(rawPrior));
}

} // namespace modernime::core
