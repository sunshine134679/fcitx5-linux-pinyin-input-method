#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    assertTrue(indexOf(page, "拟好") < std::min<std::size_t>(5, page.items.size()),
               "visible candidates include a distinct short homophone option");

    std::size_t fullSentenceVariants = 0;
    const auto firstPageSize = std::min<std::size_t>(5, page.items.size());
    for (std::size_t index = 0; index < firstPageSize; ++index) {
        if (page.items[index].fullPinyin == "ni'hao'a'lao'di") {
            ++fullSentenceVariants;
        }
    }
    assertTrue(fullSentenceVariants == 1,
               "first page is not filled with same-prefix sentence variants");

    const auto phraseIndex = indexOf(page, "你好啊");
    assertTrue(provider.select(phraseIndex),
               "phrase-prefix candidate can be selected");
    assertTrue(provider.page().rawInput == "laodi" &&
                   provider.page().preedit == "lao'di",
               "partial selection preserves the unconsumed pinyin suffix");

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

} // namespace

int main() {
    testLongInputMixesUsefulPhrasePrefixes();
    testAbbreviationPhraseOutranksRawEnglishFallback();
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
    assertTrue(reloadedLearnedIndex < reloadedLearnedProvider.page().items.size(),
               "system candidate remains after learned suppression restart");
    assertTrue(reloadedLearnedProvider.page().items[reloadedLearnedIndex].source ==
                   modernime::core::CandidateSource::Engine,
               "suppressed candidate is no longer classified as learned");
    std::filesystem::remove(learnedPath, error);
    std::filesystem::remove(learnedPath.string() + "-wal", error);
    std::filesystem::remove(learnedPath.string() + "-shm", error);

    const auto repeatedPath = testPath("repeated-selection.sqlite3");
    modernime::pinyin::PinyinDataPaths repeatedPaths;
    repeatedPaths.learningStore = repeatedPath.string();
    modernime::pinyin::PinyinCandidateProvider repeatedProvider(repeatedPaths);
    assertTrue(repeatedProvider.append("a"),
               "single-syllable input is accepted");
    assertTrue(indexOf(repeatedProvider.page(), "啊") == 1,
               "啊 starts as the second candidate");
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
    return EXIT_SUCCESS;
}
