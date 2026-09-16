#include "modernime/core/english_dictionary.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        (void)message; std::abort();
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using modernime::core::EnglishDictionary;

    // Common English words
    assertTrue(EnglishDictionary::isEnglishWord("fact"), "fact is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("good"), "good is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("apple"), "apple is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("test"), "test is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("english"), "english is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("who"), "who is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("can"), "can is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("like"), "like is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("work"), "work is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("help"), "help is recognized");

    // Case insensitivity
    assertTrue(EnglishDictionary::isEnglishWord("Fact"), "Fact (capitalized) is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("GOOD"), "GOOD (uppercase) is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("Apple"), "Apple (capitalized) is recognized");

    // Developer terms
    assertTrue(EnglishDictionary::isEnglishWord("git"), "git is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("ssh"), "ssh is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("cmake"), "cmake is recognized");
    assertTrue(EnglishDictionary::isEnglishWord("linux"), "linux is recognized");

    // Negative tests: Chinese abbreviations excluded
    assertTrue(!EnglishDictionary::isEnglishWord("bj"), "bj is excluded");
    assertTrue(!EnglishDictionary::isEnglishWord("dl"), "dl is excluded");
    assertTrue(!EnglishDictionary::isEnglishWord("wsm"), "wsm is excluded");
    assertTrue(!EnglishDictionary::isEnglishWord("yyds"), "yyds is excluded");

    // Boundary conditions
    assertTrue(!EnglishDictionary::isEnglishWord(""), "empty string is false");
    assertTrue(!EnglishDictionary::isEnglishWord("a"), "single char is false");
    assertTrue(!EnglishDictionary::isEnglishWord("123"), "numbers are false");
    assertTrue(!EnglishDictionary::isEnglishWord("fact!"), "punctuation is false");
    assertTrue(!EnglishDictionary::isEnglishWord("nonexistentwordxyz"), "nonsense word is false");

    return EXIT_SUCCESS;
}
