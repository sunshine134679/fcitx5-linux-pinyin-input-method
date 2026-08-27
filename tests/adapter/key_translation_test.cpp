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
    assertTrue(!modernime::fcitx5::translateKey(fcitx::Key(FcitxKey_Delete))
                    .has_value(),
               "plain delete remains unhandled");
    assertKind(FcitxKey_Up, modernime::fcitx5::KeyKind::PreviousPage,
               "up maps to previous page");
    assertKind(FcitxKey_Page_Up, modernime::fcitx5::KeyKind::PreviousPage,
               "page up maps to previous page");
    assertKind(FcitxKey_Down, modernime::fcitx5::KeyKind::NextPage,
               "down maps to next page");
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
