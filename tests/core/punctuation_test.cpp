#include "modernime/core/punctuation.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "punctuation test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void testFixedMappings() {
    assertTrue(modernime::core::fullWidthPunctuation(',') ==
                   std::optional<std::string>("，"),
               "comma converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation('.') ==
                   std::optional<std::string>("。"),
               "period converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation('?') ==
                   std::optional<std::string>("？"),
               "question mark converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation('!') ==
                   std::optional<std::string>("！"),
               "exclamation converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation(':') ==
                   std::optional<std::string>("："),
               "colon converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation(';') ==
                   std::optional<std::string>("；"),
               "semicolon converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation('(') ==
                   std::optional<std::string>("（"),
               "left parenthesis converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation(')') ==
                   std::optional<std::string>("）"),
               "right parenthesis converts to full-width");
    assertTrue(modernime::core::fullWidthPunctuation('~') ==
                   std::optional<std::string>("～"),
               "tilde converts to full-width");
}

void testUnmappedCharactersStayHalfWidth() {
    for (const char ascii : {'@', '#', '$', '/', '\\', '+', '=', '&', '%'}) {
        assertTrue(!modernime::core::fullWidthPunctuation(ascii).has_value(),
                   "characters without a mapping have no conversion");
    }
    assertTrue(!modernime::core::fullWidthPunctuation('a').has_value(),
               "letters are not punctuation");
    assertTrue(!modernime::core::fullWidthPunctuation('3').has_value(),
               "digits are not punctuation");
}

void testQuoteConstantsArePaired() {
    assertTrue(modernime::core::kLeftDoubleQuote != modernime::core::kRightDoubleQuote,
               "double quotes have distinct open and close forms");
    assertTrue(modernime::core::kLeftSingleQuote != modernime::core::kRightSingleQuote,
               "single quotes have distinct open and close forms");
}

} // namespace

int main() {
    testFixedMappings();
    testUnmappedCharactersStayHalfWidth();
    testQuoteConstantsArePaired();
    return EXIT_SUCCESS;
}
