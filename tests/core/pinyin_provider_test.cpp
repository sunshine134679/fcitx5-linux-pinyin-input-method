#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/candidate_mixer.h"

#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "pinyin provider test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testPath(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-" + std::string(name));
    std::error_code error;
    std::filesystem::remove(path, error);
    return path;
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::ofstream output(path);
    output << contents;
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

bool hasText(const modernime::core::CandidatePage &page,
             std::string_view text) {
    return indexOf(page, text) < page.items.size();
}

void testMixerUsesRawFallbackWhenNoHomophoneExists() {
    const std::string rawPinyin = "jiayibingding";
    const std::vector<modernime::core::CandidateItem> fullCandidates{
        {.text = "甲乙丙丁",
         .fullPinyin = "jia'yi'bing'ding",
         .sourceIndex = 10,
         .source = modernime::core::CandidateSource::Engine,
         .consumedInputBytes = 13},
        {.text = "甲乙丙钉",
         .fullPinyin = "jia'yi'bing'ding",
         .sourceIndex = 11,
         .source = modernime::core::CandidateSource::Engine,
         .consumedInputBytes = 13},
        {.text = "甲乙丙订",
         .fullPinyin = "jia'yi'bing'ding",
         .sourceIndex = 12,
         .source = modernime::core::CandidateSource::Engine,
         .consumedInputBytes = 13},
    };
    const std::vector<modernime::core::CandidateItem> partialCandidates{
        {.text = "旁",
         .fullPinyin = "pang",
         .sourceIndex = 20,
         .source = modernime::core::CandidateSource::Engine,
         .consumedInputBytes = 3},
    };

    const auto mixed = modernime::pinyin::mixCandidateItems(
        fullCandidates, partialCandidates, &fullCandidates.front(),
        {3, 5, 9}, rawPinyin);

    assertTrue(mixed.size() >= 6,
               "missing homophone fallback retains deferred candidates");
    assertTrue(mixed[4].source == modernime::core::CandidateSource::Raw &&
                   mixed[4].text == rawPinyin,
               "raw pinyin fills the fifth slot when no homophone qualifies");
    assertTrue(mixed[5].text == "甲乙丙订",
               "the third full sentence is deferred beyond the top five");

    std::size_t topFiveFullSentences = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        if (mixed[index].source != modernime::core::CandidateSource::Raw &&
            mixed[index].consumedInputBytes == rawPinyin.size()) {
            ++topFiveFullSentences;
        }
    }
    assertTrue(topFiveFullSentences == 2,
               "the first five contain exactly two full sentences");
}

void testLongInputMixesUsefulPhrasePrefixes() {
    const auto learningPath = testPath("multigranularity-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    assertTrue(provider.append("nihaoalaodi"),
               "long continuous pinyin is accepted");
    const auto &page = provider.page();
    assertTrue(!page.items.empty() && page.items.front().text == "你好啊老弟",
               "best full-sentence conversion stays first");
    assertTrue(indexOf(page, "你好啊") < std::min<std::size_t>(9, page.items.size()),
               "first page includes a useful three-syllable prefix");
    assertTrue(indexOf(page, "你好") < std::min<std::size_t>(9, page.items.size()),
               "first page includes a useful two-syllable prefix");
    assertTrue(indexOf(page, "拟") < std::min<std::size_t>(5, page.items.size()),
               "visible candidates include a real decoded homophone option");

    std::size_t fullSentenceVariants = 0;
    const auto firstPageSize = std::min<std::size_t>(5, page.items.size());
    for (std::size_t index = 0; index < firstPageSize; ++index) {
        if (page.items[index].fullPinyin == "ni'hao'a'lao'di") {
            ++fullSentenceVariants;
        }
    }
    assertTrue(fullSentenceVariants <= 2,
               "first page keeps at most two full-sentence variants");

    const auto phraseIndex = indexOf(page, "你好啊");
    assertTrue(provider.select(phraseIndex),
               "phrase-prefix candidate can be selected");
    assertTrue(provider.page().rawInput == "laodi" &&
                   provider.page().preedit == "lao'di",
               "partial selection preserves the unconsumed pinyin suffix");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
}

void testLongInputMixingPrioritizesTrustedPrefixesAndSentences() {
    const auto learningPath = testPath("quality-driven-mixing-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    assertTrue(provider.append("qingbangwodakaiwenjian"),
               "long request is accepted");
    assertTrue(provider.page().items.front().text == "请帮我打开文件",
               "best sentence stays first");
    assertTrue(indexOf(provider.page(), "请帮我") < 5,
               "trusted three-character prefix is visible");
    assertTrue(indexOf(provider.page(), "请帮") < 5,
               "trusted two-character prefix is visible");

    std::size_t noisyTwoCharacterCandidates = 0;
    const auto visibleCount =
        std::min<std::size_t>(5, provider.page().items.size());
    for (std::size_t index = 0; index < visibleCount; ++index) {
        const auto &text = provider.page().items[index].text;
        if (text == "青帮" || text == "清帮" || text == "青棒") {
            ++noisyTwoCharacterCandidates;
        }
    }
    assertTrue(noisyTwoCharacterCandidates <= 1,
               "at most one noisy two-character homophone reaches the top five");

    provider.reset();
    assertTrue(provider.append("nengbunengbangwokanxia"),
               "ambiguous sentence is accepted");
    assertTrue(indexOf(provider.page(), "能不能帮我看下") < 3,
               "semantic sentence reaches top three");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
}

void testExtensionDictionaryIsLoadedAsOfflineKnowledge() {
    const auto learningPath = testPath("extension-knowledge-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("qiangliantongsuodian"),
               "full pinyin from the extension dictionary is accepted");
    const auto index = indexOf(provider.page(), "强连通缩点");
    assertTrue(index < provider.page().items.size(),
               "an offline IT term is decoded from the extension dictionary");
    assertTrue(provider.page().items[index].source ==
                   modernime::core::CandidateSource::Engine,
               "offline knowledge stays an engine candidate in this phase");

    modernime::pinyin::PinyinDataPaths missingPaths;
    missingPaths.extensionDictionary =
        (std::filesystem::temp_directory_path() /
         "modernime-extension-dictionary-does-not-exist").string();
    missingPaths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider fallbackProvider(missingPaths);
    assertTrue(fallbackProvider.append("nihao"),
               "missing offline knowledge does not block provider startup");
    assertTrue(hasText(fallbackProvider.page(), "你好"),
               "system dictionary remains usable without offline knowledge");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testAbbreviationPhraseOutranksRawEnglishFallback() {
    const auto learningPath = testPath("abbreviation-idiom-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("hstz"),
               "idiom abbreviation input is accepted");
    assertTrue(!provider.page().items.empty(),
               "abbreviation input produces candidates");
    assertTrue(provider.page().items.front().text == "画蛇添足",
               "an idiom abbreviation outranks the raw English fallback");
    assertTrue(provider.page().items.front().source !=
                   modernime::core::CandidateSource::Raw,
               "an idiom abbreviation is not classified as raw English");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testInternetHotwordsTopRanking() {
    const auto learningPath = testPath("hotwords-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);

    assertTrue(provider.append("yyds"),
               "hotword abbreviation input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "永远的神",
               "hotword yyds outranks system dictionary combinations");

    provider.reset();
    assertTrue(provider.append("xyb"),
               "hotword abbreviation xyb is accepted");
    // 三字母输入保留英文 fallback 置顶（与 "who" 行为一致），
    // 热词应紧随其后进入前二。
    assertTrue(indexOf(provider.page(), "显眼包") < 2,
               "hotword xyb reaches top two behind the short raw fallback");

    provider.reset();
    assertTrue(provider.append("xswl"),
               "hotword abbreviation xswl is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "笑死我了",
               "hotword xswl ranks first");

    provider.reset();
    assertTrue(provider.append("yongyuandeshen"),
               "hotword full pinyin input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "永远的神",
               "hotword full pinyin keeps ranking first");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testCommonFuzzyAndShortAbbreviationRanking() {
    const auto learningPath = testPath("fuzzy-abbreviation-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    modernime::pinyin::PinyinCandidateProvider provider(paths, options);

    assertTrue(provider.append("zongguo"),
               "fuzzy initial input is accepted");
    assertTrue(!provider.page().items.empty() &&
                   modernime::core::PinyinMatchPolicy::priority(
                       "zongguo",
                       provider.page().items.front().fullPinyin) == 2,
               "an exact pinyin candidate remains ahead of fuzzy matches");
    const auto chinaIndex = indexOf(provider.page(), "中国");
    assertTrue(chinaIndex < provider.page().items.size() && chinaIndex < 3,
               "z/zh fuzzy match is recoverable near the front");
    const auto exactBeforeChina = std::count_if(
        provider.page().items.begin(),
        provider.page().items.begin() + chinaIndex,
        [](const auto &item) {
            return modernime::core::PinyinMatchPolicy::priority(
                       "zongguo", item.fullPinyin) == 2;
        });
    assertTrue(exactBeforeChina == 1,
               "one exact candidate stays before the native-best fuzzy item");
    const auto exactAfterChina = std::count_if(
        provider.page().items.begin() + chinaIndex + 1,
        provider.page().items.end(),
        [](const auto &item) {
            return modernime::core::PinyinMatchPolicy::priority(
                       "zongguo", item.fullPinyin) == 2;
        });
    assertTrue(exactAfterChina > 0,
               "exact homophone variants remain after the fuzzy item");

    provider.reset();
    assertTrue(provider.append("wsm"),
               "common short abbreviation is accepted");
    assertTrue(hasText(provider.page(), "为什么"),
               "common short abbreviation has a real LibIME candidate");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "为什么",
               "common short abbreviation outranks raw letters");
    assertTrue(indexOf(provider.page(), "wsm") < provider.page().items.size(),
               "raw short abbreviation remains available");

    provider.reset();
    assertTrue(provider.append("who"), "ordinary English word is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "who" &&
                   provider.page().items.front().source ==
                       modernime::core::CandidateSource::Raw,
               "ordinary English word stays raw first");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testAbbreviationInputIsAutomaticallySegmentedInPreedit() {
    const auto learningPath = testPath("abbreviation-preedit-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("smcg"),
               "idiom initial input is accepted for automatic segmentation");
    assertTrue(hasText(provider.page(), "四面楚歌"),
               "the segmented initial input still exposes its Chinese match");
    assertTrue(provider.page().preedit == "s'm'c'g",
               "initial input is displayed with automatic syllable separators");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testFullPinyinInputIsAutomaticallySegmentedInPreedit() {
    const auto learningPath = testPath("full-pinyin-preedit-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("wosh"),
               "unfinished full pinyin is accepted for automatic segmentation");
    assertTrue(hasText(provider.page(), "我是"),
               "unfinished full pinyin exposes its Chinese match");
    assertTrue(provider.page().preedit == "wo'sh",
               "unfinished final syllable keeps the discovered boundary");

    provider.reset();
    assertTrue(provider.append("woshishenme"),
               "continuous full pinyin is accepted for automatic segmentation");
    assertTrue(hasText(provider.page(), "我是什么"),
               "continuous full pinyin exposes its Chinese match");
    assertTrue(provider.page().preedit == "wo'shi'shen'me",
               "full pinyin is displayed with candidate syllable separators");

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testTypoCorrectionTransposition() {
    const auto learningPath = testPath("typo-transposition-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);

    // 1. yuedign -> 约定 (Rank 0), preedit -> yue'dign
    assertTrue(provider.append("yuedign"), "yuedign input accepted");
    assertTrue(!provider.page().items.empty(), "yuedign has candidates");
    assertTrue(provider.page().items.front().text == "约定",
               "yuedign typo correction ranks 约定 at index 0");
    assertTrue(provider.page().items.front().consumedInputBytes == 7,
               "约定 consumes full 7 bytes of yuedign");
    assertTrue(provider.page().preedit == "yue'dign",
               "yuedign preedit aligns to yue'dign");
    provider.reset();

    // 2. xiagn -> 向 or 想 (Rank 0)
    assertTrue(provider.append("xiagn"), "xiagn input accepted");
    assertTrue(!provider.page().items.empty(), "xiagn has candidates");
    assertTrue(provider.page().items.front().text == "向" ||
               provider.page().items.front().text == "想",
               "xiagn typo correction ranks 向/想 at index 0");
    provider.reset();

    // 3. zhogn -> 中 (Rank 0)
    assertTrue(provider.append("zhogn"), "zhogn input accepted");
    assertTrue(!provider.page().items.empty(), "zhogn has candidates");
    assertTrue(provider.page().items.front().text == "中",
               "zhogn typo correction ranks 中 at index 0");
    provider.reset();

    // 4. zhegn -> 正 (Rank 0)
    assertTrue(provider.append("zhegn"), "zhegn input accepted");
    assertTrue(!provider.page().items.empty(), "zhegn has candidates");
    assertTrue(provider.page().items.front().text == "正",
               "zhegn typo correction ranks 正 at index 0");
    provider.reset();

    // 5. English word date is not hijacked
    assertTrue(provider.append("date"), "date input accepted");
    assertTrue(!provider.page().items.empty(), "date has candidates");
    assertTrue(provider.page().items.front().text == "date",
               "date remains at rank 0");
    provider.reset();

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testNumeralMacrosAtZeroGroupBoundaries() {
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;
    options.contextLearningEnabled = false;
    modernime::pinyin::PinyinCandidateProvider provider(
        modernime::pinyin::PinyinDataPaths{}, options);
    struct NumeralCase {
        std::string_view input;
        std::string_view financial;
        std::string_view standard;
    };
    const NumeralCase cases[] = {
        {"v0", "零元整", "零"},
        {"v10000", "壹万元整", "一万"},
        {"v100000000", "壹亿元整", "一亿"},
        {"v1000000000000", "壹兆元整", "一兆"},
        {"v1000000000000.05", "壹兆元零伍分", "一兆点零五"},
        {"v1000000000001", "壹兆零壹元整", "一兆零一"},
        {"v1000000010000", "壹兆零壹万元整", "一兆零一万"},
        {"v1000100000000", "壹兆零壹亿元整", "一兆零一亿"},
        {"v1000100010001", "壹兆零壹亿零壹万零壹元整", "一兆零一亿零一万零一"},
        {"v100000000000000", "壹佰兆元整", "一百兆"},
    };
    for (const auto &test : cases) {
        provider.reset();
        assertTrue(provider.append(test.input), "numeral boundary input is accepted");
        const auto &items = provider.page().items;
        assertTrue(items.size() >= 3, "numeral conversion offers both forms and raw input");
        if (items[0].text != test.financial || items[1].text != test.standard) {
            std::cerr << "input " << test.input << ": got " << items[0].text
                      << " / " << items[1].text << '\n';
        }
        assertTrue(items[0].text == test.financial,
                   "financial numeral omits units for all-zero groups");
        assertTrue(items[1].text == test.standard,
                   "standard numeral omits units for all-zero groups");
        assertTrue(items[2].text == test.input.substr(1),
                   "numeral conversion preserves the raw number");
    }
}

void testPunctuationAndSymbolMacroCandidates() {
    const auto learningPath = testPath("macro-symbols-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.extensionDictionary = MODERNIME_PINYIN_KNOWLEDGE_BUILD_BINARY;
    paths.hotwordDictionary = MODERNIME_PINYIN_HOTWORDS_BUILD_BINARY;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);

    // 1. gantanhao -> 感叹号 at rank 0, ！ at rank 1
    assertTrue(provider.append("gantanhao"), "gantanhao input accepted");
    assertTrue(provider.page().items.size() >= 2, "gantanhao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "感叹号", "gantanhao rank 0 is 感叹号");
    assertTrue(provider.page().items[1].text == "！", "gantanhao rank 1 is ！");
    assertTrue(provider.select(1), "selecting rank 1 ！ succeeds");
    assertTrue(provider.page().items.empty(), "selecting ！ clears candidate page");
    provider.reset();

    // 2. gan'tan'hao -> 感叹号 at rank 0, ！ at rank 1
    assertTrue(provider.append("gan'tan'hao"), "gan'tan'hao input accepted");
    assertTrue(provider.page().items.size() >= 2, "gan'tan'hao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "感叹号", "gan'tan'hao rank 0 is 感叹号");
    assertTrue(provider.page().items[1].text == "！", "gan'tan'hao rank 1 is ！");
    provider.reset();

    // 3. wenhao -> 问号/问好 at rank 0, ？ at rank 1
    assertTrue(provider.append("wenhao"), "wenhao input accepted");
    assertTrue(provider.page().items.size() >= 2, "wenhao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "问好" || provider.page().items[0].text == "问号",
               "wenhao rank 0 is 问好 or 问号");
    assertTrue(provider.page().items[1].text == "？", "wenhao rank 1 is ？");
    provider.reset();

    // 4. juhao -> 句号 at rank 0, 。 at rank 1
    assertTrue(provider.append("juhao"), "juhao input accepted");
    assertTrue(provider.page().items.size() >= 2, "juhao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "句号", "juhao rank 0 is 句号");
    assertTrue(provider.page().items[1].text == "。", "juhao rank 1 is 。");
    provider.reset();

    // 5. douhao -> ， at rank 1
    assertTrue(provider.append("douhao"), "douhao input accepted");
    assertTrue(provider.page().items.size() >= 2, "douhao has at least 2 candidates");
    assertTrue(provider.page().items[1].text == "，", "douhao rank 1 is ，");
    provider.reset();

    // 6. dunhao -> 、 at rank 1
    assertTrue(provider.append("dunhao"), "dunhao input accepted");
    assertTrue(provider.page().items.size() >= 2, "dunhao has at least 2 candidates");
    assertTrue(provider.page().items[1].text == "、", "dunhao rank 1 is 、");
    provider.reset();

    // 7. shuminghao -> 书名号 at rank 0, 《》 at rank 1
    assertTrue(provider.append("shuminghao"), "shuminghao input accepted");
    assertTrue(provider.page().items.size() >= 2, "shuminghao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "书名号", "shuminghao rank 0 is 书名号");
    assertTrue(provider.page().items[1].text == "《》", "shuminghao rank 1 is 《》");
    provider.reset();

    // 8. kuohao -> 括号 at rank 0, （） at rank 1
    assertTrue(provider.append("kuohao"), "kuohao input accepted");
    assertTrue(provider.page().items.size() >= 2, "kuohao has at least 2 candidates");
    assertTrue(provider.page().items[0].text == "括号", "kuohao rank 0 is 括号");
    assertTrue(provider.page().items[1].text == "（）", "kuohao rank 1 is （）");
    provider.reset();

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testRepeatedSelectionAcrossContextsStillPromotes() {
    const auto learningPath = testPath("context-promotion-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    // Reproduces real typing: every selection carries a different piece of
    // surrounding text, so selections land in different context rows.
    for (int count = 0; count < 3; ++count) {
        provider.setContext("前文" + std::to_string(count), "后文");
        assertTrue(provider.append("a"), "single-syllable input is accepted");
        const auto index = indexOf(provider.page(), "啊");
        assertTrue(index < provider.page().items.size(),
                   "the candidate remains available");
        assertTrue(provider.select(index), "selection is accepted");
        provider.reset();
    }
    // A brand-new context that never occurred before must still see the
    // aggregated frequency.
    provider.setContext("完全", "不同");
    assertTrue(provider.append("a"), "single-syllable input is re-accepted");
    assertTrue(provider.page().items.front().text == "啊",
               "repeated selections across contexts promote the word to top");
    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testEnglishDefinitionCandidates() {
    std::error_code error;
    const auto learningPath = testPath("english-def-learning.sqlite3");
    std::filesystem::remove(learningPath, error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    modernime::pinyin::PinyinProviderOptions options;
    options.englishDefinitionEnabled = false;

    // 1. 开关关闭时：apple 第 1 位为 apple，annotation 为空，绝不插入释义候选
    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        assertTrue(provider.append("apple"), "append apple with def disabled");
        assertTrue(!provider.page().items.empty(), "has candidates");
        assertTrue(provider.page().items.front().text == "apple", "apple is at rank 0");
        assertTrue(provider.page().items.front().annotation.empty(), "annotation is empty when disabled");
        bool hasDef = false;
        for (const auto &item : provider.page().items) {
            if (item.source == modernime::core::CandidateSource::EnglishDefinition) {
                hasDef = true;
                break;
            }
        }
        assertTrue(!hasDef, "no definition candidate when switch is disabled");
    }

    // 2. 开关开启时：测试纯英文置顶 annotation、前缀补全 annotation、中文置顶抑制、单音节抑制、选词上屏与学习隔离
    options.englishDefinitionEnabled = true;
    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // A. 纯英文置顶 (apple) 携带 annotation，无独立释义候选条目
        assertTrue(provider.append("apple"), "append apple with def enabled");
        assertTrue(!provider.page().items.empty(), "at least 1 candidate for apple");
        assertTrue(provider.page().items[0].text == "apple", "apple is top candidate");
        assertTrue(provider.page().items[0].annotation == "苹果；家伙",
                   "apple annotation matches expected clean translation");
        for (const auto &item : provider.page().items) {
            assertTrue(item.source != modernime::core::CandidateSource::EnglishDefinition,
                       "no independent EnglishDefinition candidate in items");
        }

        // B. 选中首选英文单词：上屏并清空，不污染释义到学习记录
        assertTrue(provider.select(0), "select apple candidate succeeds");
        assertTrue(provider.page().items.empty(), "page cleared after select");

        // 验证学习库：没有记录 苹果；家伙
        modernime::core::LearningStore store(learningPath);
        const auto snap = store.snapshot();
        assertTrue(!snap->hasPositiveFrequency("苹果；家伙", "apple"),
                   "selecting apple did not write definition into learning store");

        provider.reset();

        // C. 前缀补全置顶 (gara -> garage) 携带 annotation，第 2 位为真实候选 gara
        assertTrue(provider.append("gara"), "append gara");
        assertTrue(provider.page().items.size() >= 2, "at least 2 candidates for gara");
        assertTrue(provider.page().items[0].text == "garage", "garage is top candidate for gara");
        assertTrue(provider.page().items[0].annotation == "车库；汽车修理厂",
                   "garage annotation matches expected translation");
        assertTrue(provider.page().items[1].text == "gara",
                   "second candidate is real candidate gara without slot hijacking");

        // 选择第 2 项上屏应得到 gara
        assertTrue(provider.select(1), "select second candidate gara succeeds");
        assertTrue(provider.page().items.empty(), "page cleared after selecting second candidate");

        provider.reset();

        // D. 中文置顶抑制 (like -> 立刻)
        assertTrue(provider.append("like"), "append like");
        assertTrue(!provider.page().items.empty(), "candidates not empty for like");
        assertTrue(provider.page().items.front().text == "立刻", "立刻 is top candidate for like");
        assertTrue(provider.page().items.front().annotation.empty(),
                   "like does not attach definition when Chinese is at top");

        provider.reset();

        // E. 单音节拼音英文沉底抑制 (de -> 的)
        assertTrue(provider.append("de"), "append de");
        assertTrue(!provider.page().items.empty(), "candidates not empty for de");
        assertTrue(provider.page().items.front().text == "的", "的 is top candidate for de");
        assertTrue(provider.page().items.front().annotation.empty(),
                   "de does not attach definition when single-syllable Chinese is at top");

        provider.reset();

        // F. 动态运行时切换 setEnglishDefinitionEnabled
        provider.setEnglishDefinitionEnabled(false);
        assertTrue(provider.append("apple"), "append apple after disabling definition at runtime");
        assertTrue(!provider.page().items.empty() && provider.page().items[0].text == "apple",
                   "apple is top candidate");
        assertTrue(provider.page().items[0].annotation.empty(),
                   "no annotation after setEnglishDefinitionEnabled(false)");

        provider.setEnglishDefinitionEnabled(true);
        assertTrue(provider.page().items[0].annotation == "苹果；家伙",
                   "annotation reappears dynamically upon setEnglishDefinitionEnabled(true)");

        // G. 短简拼中文保护与释义抑制 (sm -> 什么, gn -> 中文, zw -> 中文/作为)
        provider.reset();
        assertTrue(provider.append("sm"), "append sm");
        assertTrue(!provider.page().items.empty(), "has candidates for sm");
        assertTrue(provider.page().items.front().text == "什么", "sm top candidate is 什么");
        assertTrue(provider.page().items.front().annotation.empty(),
                   "sm does not attach definition when Chinese is at top");

        provider.reset();
        assertTrue(provider.append("gn"), "append gn");
        assertTrue(!provider.page().items.empty(), "has candidates for gn");
        assertTrue(provider.page().items.front().source != modernime::core::CandidateSource::Raw,
                   "gn top candidate is Chinese, not raw/predicted English");
        assertTrue(provider.page().items.front().text != "gnp", "gn does not predict gnp as top");
        assertTrue(provider.page().items.front().annotation.empty(),
                   "gn does not attach definition when Chinese is at top");

        provider.reset();
        assertTrue(provider.append("zw"), "append zw");
        assertTrue(!provider.page().items.empty(), "has candidates for zw");
        assertTrue(provider.page().items.front().source != modernime::core::CandidateSource::Raw,
                   "zw top candidate is Chinese, not raw/predicted English");
        assertTrue(provider.page().items.front().text != "zwingli", "zw does not predict zwingli as top");
        assertTrue(provider.page().items.front().annotation.empty(),
                   "zw does not attach definition when Chinese is at top");
    }

    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testPinyinDataPathsDetectionAndDiagnostics() {
    // 1. 验证默认探测出的路径非空
    const auto detectedDict = modernime::pinyin::defaultDictionaryPath();
    const auto detectedLM = modernime::pinyin::defaultLanguageModelPath();
    assertTrue(!detectedDict.empty(), "defaultDictionaryPath must not be empty");
    assertTrue(!detectedLM.empty(), "defaultLanguageModelPath must not be empty");

    // 2. 验证环境变量覆盖探测路径
    ::setenv("MODERNIME_PINYIN_DICT", "/custom/test/sc.dict", 1);
    assertTrue(modernime::pinyin::defaultDictionaryPath() == "/custom/test/sc.dict",
               "MODERNIME_PINYIN_DICT overrides defaultDictionaryPath");
    ::unsetenv("MODERNIME_PINYIN_DICT");

    ::setenv("MODERNIME_PINYIN_LM", "/custom/test/zh_CN.lm", 1);
    assertTrue(modernime::pinyin::defaultLanguageModelPath() == "/custom/test/zh_CN.lm",
               "MODERNIME_PINYIN_LM overrides defaultLanguageModelPath");
    ::unsetenv("MODERNIME_PINYIN_LM");

    // 3. 验证缺失路径下的优雅降级与构造（不应抛异常或崩溃，并输出警告）
    modernime::pinyin::PinyinDataPaths missingPaths;
    missingPaths.dictionary = "/tmp/nonexistent-modernime-test.dict";
    missingPaths.languageModel = "/tmp/nonexistent-modernime-test.lm";
    missingPaths.learningStore = testPath("diagnostics-test.sqlite3").string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = false;

    std::stringstream cerrBuffer;
    std::streambuf *oldCerr = std::cerr.rdbuf(cerrBuffer.rdbuf());
    modernime::pinyin::PinyinCandidateProvider provider(missingPaths, options);
    std::cerr.rdbuf(oldCerr);

    const auto warningLog = cerrBuffer.str();
    assertTrue(warningLog.find("ModernIME 警告: 拼音系统词典文件不存在") != std::string::npos,
               "missing dictionary triggers explicit warning log");
    assertTrue(warningLog.find("ModernIME 警告: 拼音语言模型文件不存在") != std::string::npos,
               "missing language model triggers explicit warning log");

    std::error_code error;
    std::filesystem::remove(missingPaths.learningStore, error);
}

} // namespace

void testVCommandMenuAndDeduplication() {
    modernime::pinyin::PinyinCandidateProvider provider;
    assertTrue(provider.append("v"), "v input accepted");
    auto page = provider.page();
    assertTrue(page.items.size() == 2, "v produces exactly 2 candidates (raw + clipboard command)");
    assertTrue(page.items[0].text == "v" &&
               page.items[0].source == modernime::core::CandidateSource::Raw,
               "candidate 0 is raw v");
    assertTrue(page.items[1].text == "剪贴板 (V+2)" &&
               page.items[1].source == modernime::core::CandidateSource::Command,
               "candidate 1 is clipboard command with trigger");

    std::size_t vCount = 0;
    for (const auto &item : page.items) {
        if (item.text == "v") {
            ++vCount;
        }
    }
    assertTrue(vCount == 1, "no duplicate raw v candidate");

    provider.setClipboardEnabled(false);
    page = provider.page();
    assertTrue(page.items.size() == 1 && page.items[0].text == "v",
               "disabled clipboard shows only raw v without command");

    provider.setClipboardEnabled(true);
    provider.setClipboardTrigger("V+3");
    page = provider.page();
    assertTrue(page.items.size() == 2 && page.items[1].text == "剪贴板 (V+3)",
               "custom trigger V+3 updates command text");

    assertTrue(provider.select(1), "selecting command succeeds");
    assertTrue(provider.page().items.empty(), "selecting command clears candidates");
}

void testIndependentRecallInCandidateProvider() {
    std::error_code err;
    const auto learningPath = testPath("independent-recall-provider.sqlite3");
    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);

    {
        modernime::core::LearningStore store(learningPath);
        assertTrue(store.open(), "open store to seed learned entry");
        for (int i = 0; i < 5; ++i) {
            assertTrue(store.recordSelection("深空测控", "shen'kong'ce'kong", "", "", 1000 + i),
                       "record personal learned phrase");
        }
        store.close();
    }

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        assertTrue(provider.append("shenkongcekong"), "append full pinyin");
        assertTrue(hasText(provider.page(), "深空测控"),
                   "learned phrase outside native top pool is independently recalled");
        const auto idx = indexOf(provider.page(), "深空测控");
        assertTrue(provider.page().items[idx].source == modernime::core::CandidateSource::Learned,
                   "recalled item has Learned source");
        assertTrue(provider.select(idx), "select recalled candidate");
        assertTrue(provider.page().items.empty(), "page cleared after full selection");
    }

    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);
}

void testIntraCompositionPhraseFormation() {
    std::error_code err;
    const auto learningPath = testPath("intra-composition.sqlite3");
    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        // Type full pinyin "wangshaolong"
        assertTrue(provider.append("wangshaolong"), "append wangshaolong");
        // Segment 1: select 王 (partial)
        const auto idxWang = indexOf(provider.page(), "王");
        assertTrue(idxWang < provider.page().items.size(), "found 王");
        assertTrue(provider.page().items[idxWang].consumedInputBytes == 4, "王 consumes 4 bytes");
        assertTrue(provider.select(idxWang), "select 王");
        assertTrue(provider.page().preedit == "shao'long", "preedit has remaining shao'long");

        // Segment 2: select 绍 (partial)
        const auto idxShao = indexOf(provider.page(), "绍");
        assertTrue(idxShao < provider.page().items.size(), "found 绍");
        assertTrue(provider.page().items[idxShao].consumedInputBytes == 4, "绍 consumes 4 bytes");
        assertTrue(provider.select(idxShao), "select 绍");
        assertTrue(provider.page().preedit == "long", "preedit has remaining long");

        // Segment 3: select 龙 (final)
        const auto idxLong = indexOf(provider.page(), "龙");
        assertTrue(idxLong < provider.page().items.size(), "found 龙");
        assertTrue(provider.select(idxLong), "select 龙 completes composition");
        assertTrue(provider.page().items.empty(), "page empty after full consumption");

        // Flush learning writer to guarantee persistence
        assertTrue(provider.flush(), "flush learning writer");

        // Now verify: typing wangshaolong recalls 王绍龙!
        provider.reset();
        assertTrue(provider.append("wangshaolong"), "re-enter wangshaolong");
        const auto idxFormed = indexOf(provider.page(), "王绍龙");
        assertTrue(idxFormed < provider.page().items.size(),
                   "intra-composition formed phrase 王绍龙 is recalled in candidate list");
        assertTrue(modernime::core::PinyinMatchPolicy::canonical(provider.page().items[idxFormed].fullPinyin) == "wangshaolong",
                   "formed phrase full pinyin matches wangshaolong");
        assertTrue(provider.page().items[idxFormed].consumedInputBytes == 12,
                   "formed phrase consumes all 12 input bytes");
        assertTrue(provider.select(idxFormed), "select formed phrase 王绍龙 succeeds");
    }

    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);
}

void testInterCompositionPhraseFormationAndRestart() {
    std::error_code err;
    const auto learningPath = testPath("inter-composition.sqlite3");
    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        // Repeat 5 times: wang -> 王, shao -> 绍, long -> 龙
        for (int i = 0; i < 5; ++i) {
            assertTrue(provider.append("wang"), "append wang");
            const auto idxWang = indexOf(provider.page(), "王");
            assertTrue(idxWang < provider.page().items.size(), "found 王");
            assertTrue(provider.select(idxWang), "select 王");

            assertTrue(provider.append("shao"), "append shao");
            const auto idxShao = indexOf(provider.page(), "绍");
            assertTrue(idxShao < provider.page().items.size(), "found 绍");
            assertTrue(provider.select(idxShao), "select 绍");

            assertTrue(provider.append("long"), "append long");
            const auto idxLong = indexOf(provider.page(), "龙");
            assertTrue(idxLong < provider.page().items.size(), "found 龙");
            assertTrue(provider.select(idxLong), "select 龙");
        }

        assertTrue(provider.flush(), "flush after 5 repetitions");

        // Verify that wangshaolong top candidate is 王绍龙!
        provider.reset();
        assertTrue(provider.append("wangshaolong"), "append wangshaolong");
        assertTrue(!provider.page().items.empty(), "page not empty for wangshaolong");
        assertTrue(provider.page().items.front().text == "王绍龙",
                   "wangshaolong top candidate is 王绍龙 after 5 rounds");
        assertTrue(provider.page().items.front().source == modernime::core::CandidateSource::Learned,
                   "王绍龙 source is Learned");
        assertTrue(provider.select(0), "select 王绍龙");
        assertTrue(provider.flush(), "flush before restart");
    }

    // Simulate restart: open brand new provider with the same DB
    {
        modernime::pinyin::PinyinCandidateProvider restarted(paths, options);
        assertTrue(restarted.append("wangshaolong"), "append wangshaolong in restarted provider");
        assertTrue(!restarted.page().items.empty(), "restarted page not empty");
        assertTrue(restarted.page().items.front().text == "王绍龙",
                   "wangshaolong top candidate is STILL 王绍龙 after restart");
        assertTrue(restarted.select(0), "select 王绍龙 succeeds after restart");
    }

    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);
}

void testHouxuanlanPhraseFormation() {
    std::error_code err;
    const auto learningPath = testPath("houxuanlan.sqlite3");
    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        // Repeat 5 times: houxuan -> 候选, lan -> 栏
        for (int i = 0; i < 5; ++i) {
            assertTrue(provider.append("houxuan"), "append houxuan");
            const auto idxHx = indexOf(provider.page(), "候选");
            assertTrue(idxHx < provider.page().items.size(), "found 候选");
            assertTrue(provider.select(idxHx), "select 候选");

            assertTrue(provider.append("lan"), "append lan");
            const auto idxLan = indexOf(provider.page(), "栏");
            assertTrue(idxLan < provider.page().items.size(), "found 栏");
            assertTrue(provider.select(idxLan), "select 栏");
        }

        assertTrue(provider.flush(), "flush after 5 rounds");

        // Verify that houxuanlan recalls 候选栏!
        provider.reset();
        assertTrue(provider.append("houxuanlan"), "append houxuanlan");
        assertTrue(!provider.page().items.empty(), "page not empty for houxuanlan");
        const auto idxHxl = indexOf(provider.page(), "候选栏");
        assertTrue(idxHxl < provider.page().items.size(), "候选栏 is recalled in candidate list");
        assertTrue(provider.page().items.front().text == "候选栏",
                   "houxuanlan top candidate is 候选栏 after training");
        assertTrue(provider.select(idxHxl), "select 候选栏 succeeds");
    }

    std::filesystem::remove(learningPath, err);
    std::filesystem::remove(learningPath.string() + "-wal", err);
    std::filesystem::remove(learningPath.string() + "-shm", err);
}

void testConditionalContextAssociation() {
    const auto learningPath = testPath("context-association-provider.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;
    options.contextLearningEnabled = true;

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);

        // Initially in empty context: shao does NOT have 绍 at rank 0 (usually 少/稍)
        assertTrue(provider.append("shao"), "shao input accepted");
        assertTrue(!provider.page().items.empty(), "shao has candidates");
        const std::string defaultShaoTop = provider.page().items.front().text;
        assertTrue(defaultShaoTop != "绍", "default shao top candidate is not 绍");
        provider.reset();

        // 1. Context association training:
        // Type wang, select 王
        assertTrue(provider.append("wang"), "wang input accepted");
        const auto wangIdx = indexOf(provider.page(), "王");
        assertTrue(wangIdx < provider.page().items.size(), "王 exists in wang");
        assertTrue(provider.select(wangIdx), "select 王");
        provider.reset();

        // Next input: shao. Because of fallback buffer, contextBefore is "王"!
        assertTrue(provider.append("shao"), "shao input accepted");
        const auto shaoIdx = indexOf(provider.page(), "绍");
        assertTrue(shaoIdx < provider.page().items.size(), "绍 exists in shao");
        assertTrue(provider.select(shaoIdx), "select 绍 in context 王");
        provider.reset();

        // Next input: long. Because of fallback buffer, contextBefore is "王绍"!
        assertTrue(provider.append("long"), "long input accepted");
        const auto longIdx = indexOf(provider.page(), "龙");
        assertTrue(longIdx < provider.page().items.size(), "龙 exists in long");
        assertTrue(provider.select(longIdx), "select 龙 in context 王绍");
        provider.reset();

        provider.flush();

        // 2. Acceptance verification:
        // A. Without surrounding text, typing sequence wang -> shao -> long:
        provider.resetCommitHistory(); // simulate clean start

        // Type wang, select 王
        assertTrue(provider.append("wang"), "wang input accepted");
        assertTrue(provider.select(indexOf(provider.page(), "王")), "commit 王");
        provider.reset();

        // Type shao: fallback buffer supplies context "王"!
        // 绍 must now be promoted to rank 0!
        assertTrue(provider.append("shao"), "shao input accepted");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "绍",
                   "王 + shao boosts 绍 to rank 0 via fallback context buffer");
        assertTrue(provider.select(0), "commit 绍");
        provider.reset();

        // Type long: fallback buffer supplies context "王绍"!
        // 龙 must now be promoted to rank 0!
        assertTrue(provider.append("long"), "long input accepted");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "龙",
                   "王绍 + long boosts 龙 to rank 0 via fallback context buffer");
        provider.reset();

        // B. Contrast context: unrelated or empty context does NOT unconditionally boost 绍/龙
        provider.resetCommitHistory();
        assertTrue(provider.append("shao"), "shao input in empty context");
        assertTrue(provider.page().items.front().text != "绍",
                   "unrelated/empty context does not unconditionally make 绍 rank 0");
        provider.reset();

        provider.setContext("李", "");
        assertTrue(provider.append("shao"), "shao input with context 李");
        assertTrue(provider.page().items.front().text != "绍",
                   "context 李 does not make 绍 rank 0");
        provider.reset();

        // C. With SurroundingText from application:
        provider.setContext("这是王", ""); // ends with 王
        assertTrue(provider.append("shao"), "shao with surrounding text ending in 王");
        assertTrue(provider.page().items.front().text == "绍",
                   "surrounding text ending in 王 boosts 绍 to rank 0");
        provider.reset();

        provider.setContext("我们认识王绍", ""); // ends with 王绍
        assertTrue(provider.append("long"), "long with surrounding text ending in 王绍");
        assertTrue(provider.page().items.front().text == "龙",
                   "surrounding text ending in 王绍 boosts 龙 to rank 0");
        provider.reset();
    }

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testCustomEnglishTermLearningAndPrefixCompletion() {
    const auto learningPath = testPath("custom-english-term-provider.sqlite3");
    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinProviderOptions options;
    options.learningEnabled = true;

    // 1. Initial state: gnue / gnuea does NOT predict gnueabihf
    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        assertTrue(provider.append("gnue"), "append gnue before training");
        assertTrue(provider.page().items.empty() || provider.page().items.front().text != "gnueabihf",
                   "gnue does not predict gnueabihf initially");
        provider.reset();

        // 2. Train: commit gnueabihf 5 times
        for (int i = 0; i < 5; ++i) {
            assertTrue(provider.append("gnueabihf"), "append gnueabihf");
            const auto rawIdx = indexOf(provider.page(), "gnueabihf");
            assertTrue(rawIdx < provider.page().items.size(), "gnueabihf exists in candidate list");
            assertTrue(provider.select(rawIdx), "commit gnueabihf");
            provider.reset();
        }

        provider.flush();

        // 3. Verification: prefix completion on gnu, gnue, gnuea
        assertTrue(provider.append("gnu"), "append gnu after training");
        assertTrue(!provider.page().items.empty(), "candidates exist for gnu");
        assertTrue(indexOf(provider.page(), "gnueabihf") < provider.page().items.size(),
                   "gnu candidate list contains gnueabihf");
        assertTrue(provider.page().items.front().text == "gnueabihf",
                   "gnueabihf is top predicted completion for gnu");
        provider.reset();

        assertTrue(provider.append("gnue"), "append gnue after training");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "gnueabihf",
                   "gnueabihf is top predicted completion for gnue");
        provider.reset();

        assertTrue(provider.append("gnuea"), "append gnuea after training");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "gnueabihf",
                   "gnueabihf is top predicted completion for gnuea");
        provider.reset();

        // 4. Cancellation: typing uncommitted term then reset does not learn
        assertTrue(provider.append("canceledterm"), "append canceledterm");
        provider.reset();
        provider.flush();

        assertTrue(provider.append("cance"), "append cance");
        assertTrue(indexOf(provider.page(), "canceledterm") == provider.page().items.size(),
                   "canceled term is not learned");
        provider.reset();
    }

    // 5. Restart persistence: recreate provider with same database
    {
        modernime::pinyin::PinyinCandidateProvider provider(paths, options);
        assertTrue(provider.append("gnu"), "append gnu after restart");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "gnueabihf",
                   "gnueabihf completion persists across restart for gnu");
        provider.reset();

        assertTrue(provider.append("gnue"), "append gnue after restart");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "gnueabihf",
                   "gnueabihf completion persists across restart for gnue");
        provider.reset();

        assertTrue(provider.append("gnuea"), "append gnuea after restart");
        assertTrue(!provider.page().items.empty() && provider.page().items.front().text == "gnueabihf",
                   "gnueabihf completion persists across restart for gnuea");
        provider.reset();
    }

    std::error_code error;
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testExplicitCharacterExtractionAndLearning() {
    std::error_code error;
    const auto learningPath = testPath("extract-character.sqlite3");
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    modernime::pinyin::PinyinDataPaths paths;
    paths.learningStore = learningPath.string();

    {
        modernime::pinyin::PinyinCandidateProvider provider(paths);

        // 1. 输入 jianpan，首选为 键盘
        assertTrue(provider.append("jianpan"), "append jianpan");
        assertTrue(!provider.page().items.empty(), "has candidates for jianpan");
        const auto jianpanIdx = indexOf(provider.page(), "键盘");
        assertTrue(jianpanIdx < provider.page().items.size(), "found 键盘 in candidates");

        // 2. 取首字（键）测试
        std::string firstChar;
        assertTrue(provider.extractCharacter(jianpanIdx, true, &firstChar),
                   "extract first character from 键盘 succeeds");
        assertTrue(firstChar == "键", "first character of 键盘 is 键");
        assertTrue(provider.page().preedit.empty(), "preedit cleared after extraction");
        provider.flush();

        // 3. 验证学习效果：输入 jian，键 的排序优先级提升
        assertTrue(provider.append("jian"), "append jian after extracting 键");
        assertTrue(!provider.page().items.empty(), "has candidates for jian");
        const auto jianIdx = indexOf(provider.page(), "键");
        assertTrue(jianIdx < provider.page().items.size(), "found 键 in candidates for jian");
        assertTrue(provider.page().items[jianIdx].source == modernime::core::CandidateSource::Learned,
                   "键 is learned from extraction");
        provider.reset();

        // 4. 输入 jianpan，取末字（盘）测试
        assertTrue(provider.append("jianpan"), "append jianpan again");
        const auto jianpanIdx2 = indexOf(provider.page(), "键盘");
        assertTrue(jianpanIdx2 < provider.page().items.size(), "found 键盘 in candidates");
        std::string lastChar;
        assertTrue(provider.extractCharacter(jianpanIdx2, false, &lastChar),
                   "extract last character from 键盘 succeeds");
        assertTrue(lastChar == "盘", "last character of 键盘 is 盘");
        provider.flush();

        // 5. 验证学习效果：输入 pan，盘 的排序优先级提升
        assertTrue(provider.append("pan"), "append pan after extracting 盘");
        assertTrue(!provider.page().items.empty(), "has candidates for pan");
        const auto panIdx = indexOf(provider.page(), "盘");
        assertTrue(panIdx < provider.page().items.size(), "found 盘 in candidates for pan");
        assertTrue(provider.page().items[panIdx].source == modernime::core::CandidateSource::Learned,
                   "盘 is learned from extraction");
        provider.reset();

        // 6. 异常边界与反例拒绝验证：
        // 6.1 英文候选拒绝取字（无汉字音节对齐）
        assertTrue(provider.append("apple"), "append apple");
        const auto appleIdx = indexOf(provider.page(), "apple");
        assertTrue(appleIdx < provider.page().items.size(), "found apple");
        std::string dummy = "initial";
        assertTrue(!provider.extractCharacter(appleIdx, true, &dummy),
                   "English candidate apple is rejected for character extraction");
        assertTrue(dummy == "initial", "output string untouched on rejection");
        provider.reset();

        // 6.2 越界索引拒绝
        assertTrue(provider.append("jianpan"), "append jianpan");
        assertTrue(!provider.extractCharacter(9999, true, &dummy),
                   "out-of-bounds candidate index rejected");
        provider.reset();

        // 6.3 Command 候选拒绝取字
        assertTrue(provider.append("v"), "append v for menu");
        const auto clipIdx = indexOf(provider.page(), "剪贴板");
        if (clipIdx < provider.page().items.size()) {
            assertTrue(!provider.extractCharacter(clipIdx, true, &dummy),
                       "Command candidate rejected for character extraction");
        }
        provider.reset();
    }

    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

int main() {
    testPinyinDataPathsDetectionAndDiagnostics();
    testEnglishDefinitionCandidates();
    testMixerUsesRawFallbackWhenNoHomophoneExists();
    testLongInputMixesUsefulPhrasePrefixes();
    testLongInputMixingPrioritizesTrustedPrefixesAndSentences();
    testCommonFuzzyAndShortAbbreviationRanking();
    testAbbreviationPhraseOutranksRawEnglishFallback();
    testInternetHotwordsTopRanking();
    testAbbreviationInputIsAutomaticallySegmentedInPreedit();
    testFullPinyinInputIsAutomaticallySegmentedInPreedit();
    modernime::pinyin::PinyinCandidateProvider provider;
    assertTrue(provider.append("nihao"), "ASCII pinyin is accepted");
    assertTrue(provider.page().preedit == "ni'hao",
               "full pinyin preedit uses the candidate syllable boundary");
    assertTrue(!provider.page().items.empty(), "LibIME produces candidates");
    assertTrue(provider.page().items.front().text == "你好",
               "system dictionary produces the expected top candidate");
    assertTrue(provider.page().items.front().fullPinyin == "ni'hao",
               "candidate retains full pinyin segmentation");

    provider.reset();
    assertTrue(provider.append("xign"), "a common transposition typo is accepted");
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
               "common pinyin transposition xign is corrected to xing");

    provider.reset();
    assertTrue(provider.append("nihao"), "full pinyin is accepted again");

    assertTrue(provider.eraseLast(), "last pinyin byte can be erased");
    assertTrue(provider.page().preedit == "ni'ha",
               "erase refreshes the shortened pinyin segmentation");

    provider.reset();
    assertTrue(provider.append("who"), "English letters are accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "who" &&
                   provider.page().items.front().source ==
                       modernime::core::CandidateSource::Raw,
               "English input exposes the original text as the first candidate");
    assertTrue(provider.select(0),
               "the original English candidate can be selected");
    assertTrue(provider.page().preedit.empty(),
               "selecting the English candidate clears the preedit");

    provider.reset();
    assertTrue(provider.append("fact"), "English word fact is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "fact" &&
                   provider.page().items.front().source ==
                       modernime::core::CandidateSource::Raw,
               "English input fact exposes the original text as the first candidate");
    assertTrue(provider.select(0),
               "the English candidate fact can be selected");
    assertTrue(provider.page().preedit.empty(),
               "selecting the English candidate clears the preedit");

    provider.reset();
    assertTrue(provider.append("good"), "English word good is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "good" &&
                   provider.page().items.front().source ==
                       modernime::core::CandidateSource::Raw,
               "English input good exposes the original text as the first candidate");
    assertTrue(provider.select(0),
               "the English candidate good can be selected");
    assertTrue(provider.page().preedit.empty(),
               "selecting the English candidate clears the preedit");

    provider.reset();
    assertTrue(provider.append("date"), "English word date is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text == "date" &&
                   provider.page().items.front().source ==
                       modernime::core::CandidateSource::Raw,
               "date defaults to the top candidate over synthetic Chinese syllables");
    assertTrue(provider.page().items.size() > 1 &&
                   provider.page().items[1].source !=
                       modernime::core::CandidateSource::Raw,
               "synthetic Chinese syllable sequence remains as secondary candidate");
    assertTrue(provider.page().preedit == "date",
               "clean English word is shown in preedit instead of da'te");
    assertTrue(provider.select(0), "date can be selected");
    assertTrue(provider.page().preedit.empty(),
               "selecting date clears preedit");

    provider.reset();
    assertTrue(provider.append("like"), "dual-attribute word like is accepted");
    assertTrue(!provider.page().items.empty() &&
                   provider.page().items.front().text != "like" &&
                   provider.page().items.front().source !=
                       modernime::core::CandidateSource::Raw,
               "like defaults to Chinese candidate because 立刻 is a real dictionary word");
    const auto likeRawIndex = indexOf(provider.page(), "like");
    assertTrue(likeRawIndex == 1, "English word like is the second candidate initially");

    provider.reset();
    assertTrue(provider.append("woshin"),
               "in-progress pinyin input is accepted");
    const auto inProgressRawIndex = indexOf(provider.page(), "woshin");
    assertTrue(inProgressRawIndex > 0 &&
                   inProgressRawIndex < provider.page().items.size(),
               "raw in-progress pinyin stays after Chinese candidates");
    assertTrue(provider.page().items.front().source !=
                   modernime::core::CandidateSource::Raw,
               "Chinese conversion stays ahead of in-progress raw letters");

    provider.reset();
    assertTrue(provider.append("nishism"),
               "mixed full-pinyin and abbreviation input is accepted");
    const auto mixedInputRawIndex = indexOf(provider.page(), "nishism");
    assertTrue(mixedInputRawIndex > 0 &&
                   mixedInputRawIndex < provider.page().items.size(),
               "raw mixed pinyin stays after Chinese candidates");
    assertTrue(provider.page().items.front().source !=
                   modernime::core::CandidateSource::Raw,
               "Chinese conversion stays ahead of mixed pinyin letters");

    provider.reset();
    assertTrue(provider.append("xi'an"), "apostrophe separates pinyin syllables");
    assertTrue(provider.page().preedit == "xi'an",
               "pinyin separator remains in the preedit");
    provider.reset();
    assertTrue(provider.page().preedit.empty(), "reset clears preedit");
    assertTrue(provider.page().items.empty(), "reset clears candidates");

    assertTrue(!provider.append("'"),
               "a pinyin separator cannot start a composition");
    assertTrue(provider.append("ni"),
               "a syllable can be entered before a separator");
    assertTrue(!provider.append("''"),
               "consecutive pinyin separators are rejected");
    provider.reset();

    const auto dictionaryPath = testPath("remove-user-dictionary.txt");
    const auto learningPath = testPath("remove-user-learning.sqlite3");
    writeFile(dictionaryPath, "nihao\t人工智能\t100\n");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = dictionaryPath.string();
    paths.learningStore = learningPath.string();
    {
        modernime::pinyin::PinyinCandidateProvider customProvider(paths);
        assertTrue(customProvider.append("nihao"),
                   "custom dictionary input is accepted");
        const auto customIndex = indexOf(customProvider.page(), "人工智能");
        assertTrue(customIndex < customProvider.page().items.size(),
                   "custom phrase is visible before removal");
        assertTrue(customProvider.page().items[customIndex].source ==
                       modernime::core::CandidateSource::UserDictionary,
                   "custom phrase is marked as a user dictionary candidate");
        assertTrue(customProvider.remove(customIndex),
                   "custom phrase removal is accepted");
        assertTrue(!hasText(customProvider.page(), "人工智能"),
                   "custom phrase disappears after removal");
        assertTrue(hasText(customProvider.page(), "你好"),
                   "system candidate remains after custom removal");
    }
    modernime::pinyin::PinyinCandidateProvider reloadedProvider(paths);
    assertTrue(reloadedProvider.append("nihao"),
               "reloaded dictionary input is accepted");
    assertTrue(!hasText(reloadedProvider.page(), "人工智能"),
               "removed custom phrase stays absent after restart");
    std::error_code error;
    std::filesystem::remove(dictionaryPath, error);
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);

    const auto learnedPath = testPath("remove-learned.sqlite3");
    modernime::pinyin::PinyinDataPaths learnedPaths;
    learnedPaths.learningStore = learnedPath.string();
    {
        modernime::pinyin::PinyinCandidateProvider learnedProvider(learnedPaths);
        assertTrue(learnedProvider.append("nihao"),
                   "learned candidate input is accepted");
        assertTrue(learnedProvider.select(0), "candidate selection is accepted");
        assertTrue(learnedProvider.append("nihao"),
                   "learned candidate can be requested again");
        const auto learnedIndex = indexOf(learnedProvider.page(), "你好");
        assertTrue(learnedIndex < learnedProvider.page().items.size(),
                   "selected candidate is available for deletion");
        assertTrue(learnedProvider.page().items[learnedIndex].source ==
                       modernime::core::CandidateSource::Learned,
                   "selected candidate is marked as learned");
        assertTrue(learnedProvider.remove(learnedIndex),
                   "learned candidate removal is accepted");
        assertTrue(!hasText(learnedProvider.page(), "你好"),
                   "learned candidate is hidden after removal");
    }
    modernime::core::LearningStore learnedStore(learnedPath);
    assertTrue(learnedStore.open(), "learned feedback store reopens");
    const auto learnedSnapshot = learnedStore.snapshot();
    const auto *learnedEntry =
        learnedSnapshot->entry("你好", "nihao", {}, {});
    assertTrue(learnedEntry != nullptr && learnedEntry->negativeFeedback > 0,
               "learned deletion persists negative feedback");
    learnedStore.close();
    modernime::pinyin::PinyinCandidateProvider reloadedLearnedProvider(
        learnedPaths);
    assertTrue(reloadedLearnedProvider.append("nihao"),
               "reloaded learned input is accepted");
    const auto reloadedLearnedIndex = indexOf(reloadedLearnedProvider.page(),
                                              "你好");
    assertTrue(reloadedLearnedIndex == reloadedLearnedProvider.page().items.size(),
               "learned suppression also hides the native candidate after restart");
    std::filesystem::remove(learnedPath, error);
    std::filesystem::remove(learnedPath.string() + "-wal", error);
    std::filesystem::remove(learnedPath.string() + "-shm", error);

    const auto repeatedPath = testPath("repeated-selection.sqlite3");
    modernime::pinyin::PinyinDataPaths repeatedPaths;
    repeatedPaths.learningStore = repeatedPath.string();
    modernime::pinyin::PinyinCandidateProvider repeatedProvider(repeatedPaths);
    assertTrue(repeatedProvider.append("a"),
               "single-syllable input is accepted");
    assertTrue(indexOf(repeatedProvider.page(), "啊") == 0,
               "啊 starts as the top candidate");
    for (int count = 0; count < 5; ++count) {
        const auto currentIndex = indexOf(repeatedProvider.page(), "啊");
        assertTrue(currentIndex < repeatedProvider.page().items.size(),
                   "repeated candidate remains available");
        assertTrue(repeatedProvider.select(currentIndex),
                   "repeated candidate selection is accepted");
        repeatedProvider.reset();
        assertTrue(repeatedProvider.append("a"),
                   "single-syllable input can be re-entered");
    }
    assertTrue(repeatedProvider.page().items.front().text == "啊",
               "five repeated selections move 啊 to the top");
    std::filesystem::remove(repeatedPath, error);
    std::filesystem::remove(repeatedPath.string() + "-wal", error);
    std::filesystem::remove(repeatedPath.string() + "-shm", error);

    const auto englishLearnPath = testPath("english-learned.sqlite3");
    modernime::pinyin::PinyinDataPaths englishLearnPaths;
    englishLearnPaths.learningStore = englishLearnPath.string();
    {
        modernime::pinyin::PinyinCandidateProvider elProvider(englishLearnPaths);
        // A. 常用高频词汇（如 like -> 立刻，name -> 那么）：
        // 坚守中文优先原则，即使偶选过英文，中文高频词仍稳居首位（rank 0），防止输入法被意外篡位
        assertTrue(elProvider.append("like"), "dual-attribute word like is accepted");
        const auto initialLikeIdx = indexOf(elProvider.page(), "like");
        assertTrue(initialLikeIdx == 1, "like starts at index 1");
        assertTrue(elProvider.select(initialLikeIdx), "select English like");
        elProvider.reset();
        assertTrue(elProvider.append("like"), "like re-entered");
        assertTrue(!elProvider.page().items.empty() &&
                   elProvider.page().items.front().text == "立刻",
                   "high-frequency Chinese word 立刻 remains at rank 0 after selecting like");
        assertTrue(indexOf(elProvider.page(), "like") == 1,
                   "like remains candidate 1");
        elProvider.reset();

        // B. 验证 name -> 那么：常用高频词汇即使偶选过英文 name，依旧优先表示词语“那么”而不是英语
        assertTrue(elProvider.append("name"), "dual-attribute word name is accepted");
        assertTrue(elProvider.page().items.front().text == "那么",
                   "name rank 0 is 那么");
        const auto initialNameIdx = indexOf(elProvider.page(), "name");
        assertTrue(initialNameIdx == 1, "name starts at index 1");
        assertTrue(elProvider.select(initialNameIdx), "select English name");
        elProvider.reset();
        assertTrue(elProvider.append("name"), "name re-entered");
        assertTrue(!elProvider.page().items.empty() &&
                   elProvider.page().items.front().text == "那么",
                   "high-frequency Chinese word 那么 remains at rank 0 after selecting name");
        assertTrue(indexOf(elProvider.page(), "name") == 1,
                   "name remains candidate 1");
        assertTrue(elProvider.page().preedit == "na'me",
                   "segmented pinyin is preserved in preedit");
    }
    std::filesystem::remove(englishLearnPath, error);
    std::filesystem::remove(englishLearnPath.string() + "-wal", error);
    std::filesystem::remove(englishLearnPath.string() + "-shm", error);

    const auto disabledLearningPath = testPath("disabled-learning.sqlite3");
    modernime::pinyin::PinyinDataPaths disabledLearningPaths;
    disabledLearningPaths.learningStore = disabledLearningPath.string();
    modernime::pinyin::PinyinProviderOptions disabledLearningOptions;
    disabledLearningOptions.learningEnabled = false;
    modernime::pinyin::PinyinCandidateProvider disabledLearningProvider(
        disabledLearningPaths, disabledLearningOptions);
    assertTrue(disabledLearningProvider.append("nihao"),
               "learning-disabled provider accepts input");
    assertTrue(disabledLearningProvider.select(0),
               "learning-disabled provider still selects candidates");
    assertTrue(!std::filesystem::exists(disabledLearningPath),
               "disabled learning does not create a database");
    testExtensionDictionaryIsLoadedAsOfflineKnowledge();
    testRepeatedSelectionAcrossContextsStillPromotes();
    testTypoCorrectionTransposition();
    testPunctuationAndSymbolMacroCandidates();
    testNumeralMacrosAtZeroGroupBoundaries();
    testVCommandMenuAndDeduplication();
    testIndependentRecallInCandidateProvider();
    testConditionalContextAssociation();
    testCustomEnglishTermLearningAndPrefixCompletion();
    testIntraCompositionPhraseFormation();
    testInterCompositionPhraseFormationAndRestart();
    testHouxuanlanPhraseFormation();
    testExplicitCharacterExtractionAndLearning();
    return EXIT_SUCCESS;
}
