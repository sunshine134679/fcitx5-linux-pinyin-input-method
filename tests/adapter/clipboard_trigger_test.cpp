#include "modernime/fcitx5/engine.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "clipboard trigger test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

modernime::fcitx5::KeyEvent character(char value) {
    return {modernime::fcitx5::KeyKind::Character, value, 0};
}

modernime::fcitx5::KeyEvent digit(char value) {
    return {modernime::fcitx5::KeyKind::Digit, 0, value};
}

} // namespace

int main() {
    modernime::fcitx5::ClipboardTrigger trigger;

    const auto first = trigger.feed(character('v'), 100, true);
    assertTrue(first.consumed && !first.openClipboard && !first.replay,
               "first V waits for the second key");
    assertTrue(trigger.pending(), "first V creates a pending sequence");

    const auto opened = trigger.feed(digit('2'), 200, true);
    assertTrue(opened.consumed && opened.openClipboard && !opened.replay,
               "V+2 opens the clipboard sequence");
    assertTrue(!trigger.pending(), "completed sequence is cleared");

    trigger.reset();
    trigger.feed(character('v'), 100, true);
    const auto wrongSecond = trigger.feed(digit('1'), 200, true);
    assertTrue(!wrongSecond.consumed && !wrongSecond.openClipboard &&
                   wrongSecond.replay.has_value() &&
                   wrongSecond.replay->character == 'v',
               "a non-matching second key replays V");
    assertTrue(!trigger.pending(), "wrong second key clears the sequence");

    trigger.feed(character('v'), 100, true);
    const auto beforeTimeout = trigger.expire(499);
    assertTrue(!beforeTimeout.replay.has_value(),
               "pending V is retained before timeout");
    const auto afterTimeout = trigger.expire(500);
    assertTrue(afterTimeout.consumed && afterTimeout.replay.has_value() &&
                   afterTimeout.replay->character == 'v',
               "pending V is replayed after timeout");

    trigger.reset();
    const auto ineligible = trigger.feed(character('v'), 100, false);
    assertTrue(!ineligible.consumed && !trigger.pending(),
               "V is not intercepted while a preedit already exists");

    modernime::fcitx5::ClipboardTrigger custom("B+7");
    custom.feed(character('b'), 10, true);
    const auto customOpened = custom.feed(digit('7'), 20, true);
    assertTrue(customOpened.openClipboard,
               "configured letter and digit are honored");

    modernime::fcitx5::ClipboardTrigger invalid("V+0");
    const auto invalidResult = invalid.feed(character('v'), 0, true);
    assertTrue(!invalidResult.consumed && !invalid.pending(),
               "invalid trigger is disabled safely");
    return EXIT_SUCCESS;
}
