#include "modernime/ui/status_indicator.h"

#include <cstdlib>
#include <iostream>

namespace {

void assertTrue(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "status indicator test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    assertTrue(modernime::ui::StatusIndicator::iconName() ==
                   "input-keyboard-symbolic",
               "status indicator uses the keyboard icon");
    assertTrue(modernime::ui::StatusIndicator::title() == "ModernIME 拼音",
               "status indicator identifies ModernIME");
    return EXIT_SUCCESS;
}
