#include "modernime/fcitx5/fcitx_engine.h"

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
    return EXIT_SUCCESS;
}
