#include "modernime/core/english_dictionary.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << std::endl;
        std::abort();
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

    // Boundary conditions for isEnglishWord
    assertTrue(!EnglishDictionary::isEnglishWord(""), "empty string is false");
    assertTrue(!EnglishDictionary::isEnglishWord("a"), "single char is false");
    assertTrue(!EnglishDictionary::isEnglishWord("123"), "numbers are false");
    assertTrue(!EnglishDictionary::isEnglishWord("fact!"), "punctuation is false");
    assertTrue(!EnglishDictionary::isEnglishWord("nonexistentwordxyz"), "nonsense word is false");

    // Prediction tests
    {
        const auto garaPred = EnglishDictionary::predictWords("gara", 3);
        assertTrue(!garaPred.empty(), "gara yields predictions");
        assertTrue(garaPred.front() == "garage", "gara predicts garage as first candidate");
    }
    {
        const auto garagPred = EnglishDictionary::predictWords("garag", 3);
        assertTrue(!garagPred.empty(), "garag yields predictions");
        assertTrue(garagPred.front() == "garage", "garag predicts garage as first candidate");
    }
    {
        const auto applPred = EnglishDictionary::predictWords("appl", 3);
        assertTrue(!applPred.empty(), "appl yields predictions");
        assertTrue(applPred.front() == "apple", "appl predicts apple as first candidate");
    }
    {
        const auto systPred = EnglishDictionary::predictWords("syst", 3);
        assertTrue(!systPred.empty(), "syst yields predictions");
        assertTrue(systPred.front() == "system", "syst predicts system as first candidate");
    }
    {
        const auto windoPred = EnglishDictionary::predictWords("windo", 3);
        assertTrue(!windoPred.empty(), "windo yields predictions");
        assertTrue(windoPred.front() == "window", "windo predicts window as first candidate");
    }
    {
        const auto progrPred = EnglishDictionary::predictWords("progr", 3);
        assertTrue(!progrPred.empty(), "progr yields predictions");
        assertTrue(progrPred.front() == "program", "progr predicts program as first candidate");
    }

    // Boundary conditions for predictWords
    assertTrue(EnglishDictionary::predictWords("", 3).empty(), "empty prefix yields empty");
    assertTrue(EnglishDictionary::predictWords("a", 3).empty(), "single char prefix yields empty");
    assertTrue(EnglishDictionary::predictWords("123", 3).empty(), "numbers prefix yields empty");
    assertTrue(EnglishDictionary::predictWords("gara", 0).empty(), "maxCount 0 yields empty");
    assertTrue(EnglishDictionary::predictWords("nonexistentwordxyz", 3).empty(), "nonsense prefix yields empty");

    return EXIT_SUCCESS;
}
