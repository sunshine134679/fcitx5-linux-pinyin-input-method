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
    assertTrue(modernime::core::fullWidthPunctuation('/') ==
                   std::optional<std::string>("、"),
               "slash converts to full-width enumeration comma");
    assertTrue(modernime::core::fullWidthPunctuation('\\') ==
                   std::optional<std::string>("、"),
               "backslash converts to full-width enumeration comma");
    assertTrue(modernime::core::fullWidthPunctuation('<') ==
                   std::optional<std::string>("《"),
               "less-than converts to left book title mark");
    assertTrue(modernime::core::fullWidthPunctuation('>') ==
                   std::optional<std::string>("》"),
               "greater-than converts to right book title mark");
    assertTrue(modernime::core::fullWidthPunctuation('[') ==
                   std::optional<std::string>("【"),
               "left square bracket converts to full-width left bracket");
    assertTrue(modernime::core::fullWidthPunctuation(']') ==
                   std::optional<std::string>("】"),
               "right square bracket converts to full-width right bracket");
    assertTrue(modernime::core::fullWidthPunctuation('^') ==
                   std::optional<std::string>("……"),
               "caret converts to full-width ellipsis");
    assertTrue(modernime::core::fullWidthPunctuation('_') ==
                   std::optional<std::string>("——"),
               "underscore converts to full-width em-dash");
    assertTrue(modernime::core::fullWidthPunctuation('$') ==
                   std::optional<std::string>("￥"),
               "dollar converts to full-width yuan sign");
}

void testUnmappedCharactersStayHalfWidth() {
    for (const char ascii : {'@', '#', '+', '=', '&', '%'}) {
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
