#include "modernime/pinyin/pinyin_candidate_provider.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin provider test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    modernime::pinyin::PinyinCandidateProvider provider;
    assertTrue(provider.append("nihao"), "ASCII pinyin is accepted");
    assertTrue(provider.page().preedit == "nihao", "preedit follows input");
    assertTrue(!provider.page().items.empty(), "LibIME produces candidates");
    assertTrue(provider.page().items.front().text == "你好",
               "system dictionary produces the expected top candidate");
    assertTrue(provider.page().items.front().fullPinyin == "ni'hao",
               "candidate retains full pinyin segmentation");

    assertTrue(provider.eraseLast(), "last pinyin byte can be erased");
    assertTrue(provider.page().preedit == "niha", "erase refreshes preedit");
    provider.reset();
    assertTrue(provider.page().preedit.empty(), "reset clears preedit");
    assertTrue(provider.page().items.empty(), "reset clears candidates");
    return EXIT_SUCCESS;
}
