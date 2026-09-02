#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/core/pinyin_match.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin quality test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::size_t indexOf(const modernime::core::CandidatePage &page,
                    std::string_view text) {
    for (std::size_t index = 0; index < page.items.size(); ++index) {
        if (page.items[index].text == text) {
            return index;
        }
    }
    return page.items.size();
}

modernime::pinyin::PinyinCandidateProvider makeProvider() {
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    return modernime::pinyin::PinyinCandidateProvider(paths, options);
}

void testFrequentSentencesAndPartialSelection() {
    auto provider = makeProvider();

    assertTrue(provider.append("jintiantianqihenhao"),
               "weather sentence input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "今天天气很好",
               "weather sentence is the first candidate");

    provider.reset();
    assertTrue(provider.append("qingbangwodakaiwenjian"),
               "file-opening request input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "请帮我打开文件",
               "file-opening request is the first candidate");
    const auto phraseIndex = indexOf(provider.page(), "请帮我");
    assertTrue(phraseIndex < provider.page().items.size() && phraseIndex < 5,
               "trusted request prefix is in the top five");
    assertTrue(provider.select(phraseIndex),
               "trusted request prefix can be selected");
    assertTrue(provider.page().rawInput == "dakaiwenjian",
               "partial selection preserves the file-opening suffix");

    provider.reset();
    assertTrue(provider.append("nengbunengbangwokanxia"),
               "ambiguous request input is accepted");
    assertTrue(indexOf(provider.page(), "能不能帮我看下") < 3,
               "semantic ambiguous request is in the top three");
}

void testOfflineFuzzyTypoAndAbbreviationRecovery() {
    auto provider = makeProvider();

    assertTrue(provider.append("zongguo"),
               "fuzzy initial input is accepted");
    assertTrue(indexOf(provider.page(), "中国") < 3,
               "z/zh fuzzy recovery is in the top three");

    provider.reset();
    assertTrue(provider.append("wsm"),
               "common short abbreviation input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "为什么",
               "common short abbreviation is the first candidate");

    provider.reset();
    assertTrue(provider.append("who"), "ordinary English input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "who",
               "ordinary English remains the first candidate");

    provider.reset();
    assertTrue(provider.append("xign"),
               "common transposition input is accepted");
    bool foundCorrectedXing = false;
    for (const auto &candidate : provider.page().items) {
        if (candidate.source != modernime::core::CandidateSource::Raw &&
            modernime::core::PinyinMatchPolicy::canonical(
                candidate.fullPinyin) == "xing") {
            foundCorrectedXing = true;
            break;
        }
    }
    assertTrue(foundCorrectedXing,
               "transposed input exposes a corrected xing candidate");
}

} // namespace

int main() {
    testFrequentSentencesAndPartialSelection();
    testOfflineFuzzyTypoAndAbbreviationRecovery();
    return EXIT_SUCCESS;
}
