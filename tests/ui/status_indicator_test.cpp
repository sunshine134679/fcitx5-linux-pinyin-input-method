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
    assertTrue(modernime::ui::StatusIndicator::labelForInputMethod("modernime") ==
                   "中",
               "status indicator labels ModernIME as Chinese input");
    assertTrue(modernime::ui::StatusIndicator::labelForInputMethod("keyboard-us") ==
                   "英",
               "status indicator labels keyboard input as English");
    assertTrue(modernime::ui::StatusIndicator::titleForInputMethod("modernime") ==
                   "中文输入法",
               "status indicator describes Chinese input mode");
    assertTrue(modernime::ui::StatusIndicator::titleForInputMethod("keyboard-us") ==
                   "英文键盘",
               "status indicator describes English keyboard mode");
    return EXIT_SUCCESS;
}
