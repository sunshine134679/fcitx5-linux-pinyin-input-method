#include "modernime/core/dictionary_prior.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "dictionary prior test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const auto invalid = std::numeric_limits<float>::quiet_NaN();
    assertTrue(std::isfinite(modernime::core::curatedDictionaryBonus(invalid)) &&
                   modernime::core::curatedDictionaryBonus(invalid) == 0.0,
               "non-finite user dictionary cost is ignored");
    assertTrue(modernime::core::curatedDictionaryBonus(0.0F, 2) == 0.0,
               "zero-cost 2-character entries receive no curated boost");
    assertTrue(modernime::core::curatedDictionaryBonus(0.0F, 3) == 1.0,
               "zero-cost 3-character entries receive curated boost");
    assertTrue(modernime::core::curatedDictionaryBonus(5.0F, 2) > 1.0,
               "explicit positive cost 2-character entries receive curated boost");
    assertTrue(std::isfinite(modernime::core::systemDictionaryBonus(invalid)) &&
                   modernime::core::systemDictionaryBonus(invalid) == 0.0,
               "non-finite system dictionary cost is ignored");
    assertTrue(modernime::core::combinedDictionaryBonus(
                   std::numeric_limits<double>::quiet_NaN(), 0.0) == 0.0,
               "non-finite dictionary bonus is ignored");
    return EXIT_SUCCESS;
}
