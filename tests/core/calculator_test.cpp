#include "modernime/core/calculator.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void expectResult(std::string_view expression, std::string_view expected) {
    const auto result = modernime::core::calculate(expression);
    if (!result.valid || result.text != expected || !result.error.empty()) {
        std::cerr << "calculator test failed: " << expression << " expected "
                  << expected << ", got '" << result.text << "' ("
                  << result.error << ")\n";
        std::exit(EXIT_FAILURE);
    }
}

void expectInvalid(std::string_view expression) {
    const auto result = modernime::core::calculate(expression);
    if (result.valid || !result.text.empty() || result.error.empty()) {
        std::cerr << "calculator accepted invalid expression: " << expression
                  << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    // Incorrect precedence or a missing evaluator must fail this real call.
    expectResult("2+3*4", "14");
    expectResult("(2+3)*4", "20");
    expectResult(" -(.5 + +1.5) / 2 ", "-1");
    expectResult("10-3-2", "5");
    expectResult("8/4/2", "1");
    expectResult("7.5%2", "1.5");
    expectResult("-7%3", "-1");
    expectResult("0.1+0.2", "0.3");
    expectResult("1/10000000", "0.0000001");
    expectResult("2.", "2");
    expectResult("-0", "0");

    for (const auto expression : {"", ".", "1+", "1/0", "1%(2-2)",
                                  "(1+2", "2(3)", "1 2", "1+2oops"}) {
        expectInvalid(expression);
    }
    expectInvalid(std::string(513, '1'));
    expectInvalid(std::string(33, '(') + "1" + std::string(33, ')'));

    std::cout << "calculator tests passed\n";
    return EXIT_SUCCESS;
}
