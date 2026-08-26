#include "modernime/fcitx5/engine.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "engine state test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct RecordingHost final : modernime::fcitx5::EngineHost {
    std::vector<modernime::core::CandidatePage> pages;
    std::vector<std::string> commits;

    void publishPage(const modernime::core::CandidatePage &page) override {
        pages.push_back(page);
    }

    void commit(std::string_view text) override {
        commits.emplace_back(text);
    }
};

void type(modernime::fcitx5::ModernIMEController &controller,
          std::string_view text) {
    for (const char character : text) {
        assertTrue(controller.handle({
                       modernime::fcitx5::KeyKind::Character, character, 0}),
                   "character event is handled");
    }
}

} // namespace

int main() {
    RecordingHost host;
    modernime::fcitx5::ModernIMEController controller(host);

    type(controller, "hail");
    assertTrue(controller.page().preedit == "hail", "preedit is published");
    assertTrue(controller.page().items.size() == 9, "nine sample candidates exist");
    assertTrue(controller.page().items.front().text == "还",
               "first sample candidate is visible");

    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Backspace, 0, 0}),
               "backspace is handled");
    assertTrue(controller.page().preedit == "hai", "backspace removes one byte");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Escape, 0, 0}),
               "escape is handled");
    assertTrue(controller.page().preedit.empty(), "escape clears preedit");

    type(controller, "hail");
    assertTrue(controller.select(1), "candidate index selection is handled");
    assertTrue(host.commits.back() == "海", "candidate index commits second item");
    assertTrue(controller.page().preedit.empty(), "index selection clears page");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '2'}),
               "digit selection is handled");
    assertTrue(host.commits.back() == "海", "second candidate is committed");
    assertTrue(controller.page().preedit.empty(), "selection clears page");

    type(controller, "hail");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Space, 0, 0}),
               "space selects first candidate");
    assertTrue(host.commits.back() == "还", "space commits first candidate");

    type(controller, "x");
    assertTrue(controller.handle({modernime::fcitx5::KeyKind::Enter, 0, 0}),
               "enter commits raw fallback");
    assertTrue(host.commits.back() == "x", "raw fallback is committed");

    controller.handle({modernime::fcitx5::KeyKind::Toggle, 0, 0});
    assertTrue(!controller.active(), "toggle disables input");
    assertTrue(!controller.handle({modernime::fcitx5::KeyKind::Character, 'a', 0}),
               "disabled input ignores characters");
    controller.handle({modernime::fcitx5::KeyKind::Toggle, 0, 0});
    assertTrue(controller.active(), "toggle re-enables input");
    return EXIT_SUCCESS;
}
