#include "modernime/pinyin/pinyin_candidate_provider.h"

#include "modernime/core/learning_store.h"

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
    {
        modernime::pinyin::PinyinDataPaths learnedPaths;
        learnedPaths.learningStore = learnedPath.string();
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
    return EXIT_SUCCESS;
}
