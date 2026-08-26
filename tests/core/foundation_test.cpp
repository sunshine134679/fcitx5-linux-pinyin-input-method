#include <cstdlib>
#include <iostream>
#include <string_view>

int main() {
    const auto assert_true = [](bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "foundation test failed: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    };

    assert_true(true, "test harness executes");
    std::cout << "foundation test passed\n";
    return EXIT_SUCCESS;
}
