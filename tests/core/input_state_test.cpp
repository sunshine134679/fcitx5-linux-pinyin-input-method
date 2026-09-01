#include "modernime/core/candidate_model.h"
#include "modernime/core/input_state.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "input state test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    modernime::core::InputState state;
    assertTrue(state.append("hai"), "append accepts non-empty ASCII input");
    assertTrue(state.text() == "hai", "append stores input");
    assertTrue(state.generation() == 3, "append advances once per input byte");
    assertTrue(state.eraseLast(), "erase removes the final ASCII byte");
    assertTrue(state.text() == "ha", "erase updates input");
    assertTrue(state.eraseLast(), "second erase removes the final byte");
    assertTrue(state.eraseLast(), "third erase removes the final byte");
    assertTrue(!state.eraseLast(), "erase on empty input is rejected");
    assertTrue(state.replace("nihao"), "replace accepts a complete ASCII composition");
    assertTrue(state.text() == "nihao", "replace updates the whole composition");
    assertTrue(!state.replace("你") && state.text() == "nihao",
               "invalid replacement leaves the previous composition intact");

    modernime::core::CandidatePage page;
    page.preedit = "hai";
    page.items = {{"还", "hai", 0}, {"海", "hai", 1}};
    assertTrue(!page.select(2), "out-of-range selection is rejected");
    assertTrue(page.cursor == 0, "invalid selection does not move cursor");
    assertTrue(page.select(1), "valid selection is accepted");
    assertTrue(page.cursor == 1, "valid selection moves cursor");

    state.clear();
    page.clear();
    assertTrue(state.text().empty(), "clear removes input");
    assertTrue(page.items.empty(), "clear removes candidates");
    assertTrue(page.generation == 0, "cleared page has no published generation");
    return EXIT_SUCCESS;
}
