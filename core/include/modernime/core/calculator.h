#pragma once

#include <string>
#include <string_view>

namespace modernime::core {

struct CalculationResult {
    bool valid = false;
    std::string text;
    std::string error;
};

// Offline arithmetic: decimal literals, + - * / %, parentheses and unary signs.
// '%' is floating-point remainder (the result has the dividend's sign).
// Expressions are limited to 512 bytes and 32 nested parenthesis pairs.
// Uses long double and displays 15 significant decimal digits, without exponent
// notation. This is approximate arithmetic, not exact financial decimal math.
// Failure always has empty text and a nonempty error; no state or I/O is used.
CalculationResult calculate(std::string_view expression);

} // namespace modernime::core
