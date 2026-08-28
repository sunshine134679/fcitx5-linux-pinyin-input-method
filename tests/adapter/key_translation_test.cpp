#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/surroundingtext.h>
#include <fcitx-utils/keysymgen.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "key translation test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void assertKind(fcitx::KeySym symbol, modernime::fcitx5::KeyKind expected,
                std::string_view message) {
    const auto translated = modernime::fcitx5::translateKey(fcitx::Key(symbol));
    assertTrue(translated.has_value(), message);
    assertTrue(translated->kind == expected, message);
}

} // namespace

int main() {
    fcitx::SurroundingText surrounding;
    surrounding.setText("你好世界", 2, 2);
    const auto context =
        modernime::fcitx5::extractSurroundingContext(surrounding, 1);
    assertTrue(context.first == "好" && context.second == "世",
               "surrounding context is bounded by UTF-8 characters");
    surrounding.invalidate();
    const auto invalidContext =
        modernime::fcitx5::extractSurroundingContext(surrounding, 32);
    assertTrue(invalidContext.first.empty() && invalidContext.second.empty(),
               "invalid surrounding context is ignored");

    const auto ctrl = fcitx::KeyStates(fcitx::KeyState::Ctrl);
    const auto shift = fcitx::KeyStates(fcitx::KeyState::Shift);
    const modernime::fcitx5::KeyBindings defaultBindings;
    const auto defaultToggle = modernime::fcitx5::translateKey(
        fcitx::Key(FcitxKey_space, ctrl), defaultBindings);
    assertTrue(defaultToggle.has_value() &&
                   defaultToggle->kind == modernime::fcitx5::KeyKind::Toggle,
               "default toggle binding maps Ctrl+Space");
    auto alternateBindings = defaultBindings;
    alternateBindings.toggleKey = "Alt+Space";
    const auto alternateToggle = modernime::fcitx5::translateKey(
        fcitx::Key(FcitxKey_space,
                   fcitx::KeyStates(fcitx::KeyState::Alt)),
        alternateBindings);
    assertTrue(alternateToggle.has_value() &&
                   alternateToggle->kind == modernime::fcitx5::KeyKind::Toggle,
               "configured toggle binding maps Alt+Space");
    assertTrue(!modernime::fcitx5::translateKey(
                    fcitx::Key(FcitxKey_space, ctrl), alternateBindings)
                    .has_value(),
               "old toggle binding is disabled after reconfiguration");
    auto disabledBindings = defaultBindings;
    disabledBindings.numberSelection = false;
    disabledBindings.arrowNavigation = false;
    disabledBindings.pageNavigation = false;
    assertTrue(!modernime::fcitx5::translateKey(
                    fcitx::Key(FcitxKey_1), disabledBindings)
                    .has_value(),
               "disabled number selection does not translate digits");
    assertTrue(!modernime::fcitx5::translateKey(
                    fcitx::Key(FcitxKey_Up), disabledBindings)
                    .has_value(),
               "disabled page navigation does not translate arrows");
    const auto ctrlDelete = modernime::fcitx5::translateKey(
        fcitx::Key(FcitxKey_Delete, ctrl));
    assertTrue(ctrlDelete.has_value() &&
                   ctrlDelete->kind ==
                       modernime::fcitx5::KeyKind::DeleteCandidate,
               "ctrl delete maps to candidate deletion");
    const auto shiftDelete = modernime::fcitx5::translateKey(
        fcitx::Key(FcitxKey_Delete, shift));
    assertTrue(shiftDelete.has_value() &&
                   shiftDelete->kind ==
                       modernime::fcitx5::KeyKind::DeleteCandidate,
               "shift delete maps to candidate deletion");
    assertKind(FcitxKey_Delete, modernime::fcitx5::KeyKind::CloseClipboard,
               "plain delete maps to closing clipboard mode");
    assertKind(FcitxKey_Return, modernime::fcitx5::KeyKind::Enter,
               "main enter maps to candidate submission");
    assertKind(FcitxKey_KP_Enter, modernime::fcitx5::KeyKind::Enter,
               "keypad enter maps to candidate submission");
    assertKind(FcitxKey_Up, modernime::fcitx5::KeyKind::PreviousClipboardItem,
               "up maps to the previous clipboard item");
    assertKind(FcitxKey_Page_Up, modernime::fcitx5::KeyKind::PreviousPage,
               "page up maps to previous page");
    assertKind(FcitxKey_Down, modernime::fcitx5::KeyKind::NextClipboardItem,
               "down maps to the next clipboard item");
    assertKind(FcitxKey_Page_Down, modernime::fcitx5::KeyKind::NextPage,
               "page down maps to next page");
    assertKind(FcitxKey_equal, modernime::fcitx5::KeyKind::NextPage,
               "equal maps to next page");
    assertKind(FcitxKey_plus, modernime::fcitx5::KeyKind::NextPage,
               "plus maps to next page");
    const auto apostrophe = modernime::fcitx5::translateKey(
        fcitx::Key(FcitxKey_apostrophe));
    assertTrue(apostrophe.has_value() &&
                   apostrophe->kind == modernime::fcitx5::KeyKind::Character &&
                   apostrophe->character == '\'',
               "apostrophe maps to a character event");
    return EXIT_SUCCESS;
}
