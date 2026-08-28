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

    const auto first = trigger.feed(character('v'), true);
    assertTrue(first.consumed && first.openFeatureMenu &&
                   first.featurePrefix == 'V' && first.featureDigit == '2' &&
                   !first.openClipboard && !first.replay,
               "first V opens the feature menu and waits for the second key");
    assertTrue(trigger.pending(), "first V creates a pending sequence");

    const auto opened = trigger.feed(digit('2'), true);
    assertTrue(opened.consumed && opened.openClipboard && !opened.replay,
               "V+2 opens the clipboard sequence");
    assertTrue(!trigger.pending(), "completed sequence is cleared");

    trigger.reset();
    trigger.feed(character('v'), true);
    const auto wrongSecond = trigger.feed(digit('1'), true);
    assertTrue(!wrongSecond.consumed && !wrongSecond.openClipboard &&
                   wrongSecond.replay.has_value() &&
                   wrongSecond.replay->kind ==
                       modernime::fcitx5::KeyKind::CommitLiteral &&
                   wrongSecond.replay->character == 'V',
               "a non-matching second key replays V");
    assertTrue(!trigger.pending(), "wrong second key clears the sequence");

    trigger.feed(character('v'), true);
    const auto delayedOpen = trigger.feed(digit('2'), true);
    assertTrue(delayedOpen.consumed && delayedOpen.openClipboard &&
                   !delayedOpen.replay,
               "V can enter the clipboard after a long wait");
    assertTrue(!trigger.pending(), "delayed feature selection clears the trigger");

    trigger.reset();
    trigger.feed(character('v'), true);
    const auto enter = trigger.feed(
        {modernime::fcitx5::KeyKind::Enter, 0, 0}, true);
    assertTrue(enter.consumed && enter.replay.has_value() &&
                   enter.replay->kind ==
                       modernime::fcitx5::KeyKind::CommitLiteral &&
                   enter.replay->character == 'V',
               "enter commits the pending V instead of reaching the client");

    trigger.reset();
    const auto ineligible = trigger.feed(character('v'), false);
    assertTrue(!ineligible.consumed && !trigger.pending(),
               "V is not intercepted while a preedit already exists");

    modernime::fcitx5::ClipboardTrigger custom("B+7");
    custom.feed(character('b'), true);
    const auto customOpened = custom.feed(digit('7'), true);
    assertTrue(customOpened.openClipboard,
               "configured letter and digit are honored");

    modernime::fcitx5::ClipboardTrigger invalid("V+0");
    const auto invalidResult = invalid.feed(character('v'), true);
    assertTrue(!invalidResult.consumed && !invalid.pending(),
               "invalid trigger is disabled safely");
    return EXIT_SUCCESS;
}
