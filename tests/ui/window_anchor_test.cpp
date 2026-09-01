#include "modernime/ui/window_anchor.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "window anchor test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    assertTrue(modernime::ui::cursorAnchorBottom(360, 0) == 370,
               "zero-height caret still has a usable anchor below it");
    assertTrue(modernime::ui::cursorAnchorBottom(360, 22) == 382,
               "reported caret height is preserved");

    modernime::ui::WindowAnchor anchor;

    assertTrue(anchor.capture(120, 240), "first cursor position is captured");
    assertTrue(anchor.x == 120 && anchor.y == 240,
               "captured position is retained");
    assertTrue(!anchor.capture(120, 240),
               "an unchanged cursor position does not move the window");
    assertTrue(anchor.x == 120 && anchor.y == 240,
               "window remains at the same position");
    assertTrue(anchor.capture(420, 640),
               "a moved cursor repositions the window");
    assertTrue(anchor.x == 420 && anchor.y == 640,
               "window follows the new cursor position");

    anchor.reset();
    assertTrue(anchor.capture(420, 640),
               "position can be captured again after reset");
    assertTrue(anchor.x == 420 && anchor.y == 640,
               "new input starts from the new cursor position");
    return EXIT_SUCCESS;
}
