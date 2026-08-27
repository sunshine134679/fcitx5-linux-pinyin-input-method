#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/user_dictionary.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "user dictionary test failed: " << message << '\n';
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

bool hasText(const modernime::core::CandidatePage &page,
             std::string_view text) {
    for (const auto &item : page.items) {
        if (item.text == text) {
            return true;
        }
    }
    return false;
}

std::size_t countSource(const modernime::core::CandidatePage &page,
                        modernime::core::CandidateSource source) {
    std::size_t count = 0;
    for (const auto &item : page.items) {
        if (item.source == source) {
            ++count;
        }
    }
    return count;
}

void testUserDictionarySkipsBadRowsAndKeepsLastDuplicate() {
    const auto path = testPath("user-dictionary.txt");
    writeFile(path,
              "# comment\nni'hao\t你好\t100\ninvalid\n"
              "nihao\t您好\t80\nnihao\t你好\t120\n");
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(dictionary.entries().size() == 2, "valid rows are loaded");
    assertTrue(dictionary.contains("nihao", "你好"),
               "apostrophes normalize");
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testProfessionalPhraseAppearsFromConfiguredDictionary() {
    const auto dictionaryPath = testPath("professional-dictionary.txt");
    const auto learningPath = testPath("professional-learning.sqlite3");
    writeFile(dictionaryPath, "rengongzhineng\t人工智能\t100\n");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = dictionaryPath.string();
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    for (const char c : std::string("rengongzhineng")) {
        assertTrue(provider.append(std::string_view(&c, 1)),
                   "pinyin is accepted");
    }
    assertTrue(hasText(provider.page(), "人工智能"),
               "professional phrase is a candidate");
    std::error_code error;
    std::filesystem::remove(dictionaryPath, error);
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testManualCandidatesAreCappedForShortInput() {
    const auto dictionaryPath = testPath("short-input-dictionary.txt");
    const auto learningPath = testPath("short-input-learning.sqlite3");
    writeFile(dictionaryPath,
              "ni\t妮甲\t100\nni\t妮乙\t99\nni\t妮丙\t98\n"
              "ni\t妮丁\t97\nni\t妮戊\t96\nni\t妮己\t95\n");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = dictionaryPath.string();
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("n"), "short input is accepted");
    assertTrue(countSource(provider.page(),
                           modernime::core::CandidateSource::UserDictionary) <=
                   2,
               "short input keeps at most two manual candidates");
    std::error_code error;
    std::filesystem::remove(dictionaryPath, error);
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testManualCandidatesExpandForLongerInput() {
    const auto dictionaryPath = testPath("long-input-dictionary.txt");
    const auto learningPath = testPath("long-input-learning.sqlite3");
    writeFile(dictionaryPath,
              "ni'hao\t妮好甲\t100\nni'hao\t妮好乙\t99\n"
              "ni'hao\t妮好丙\t98\nni'hao\t妮好丁\t97\n"
              "ni'hao\t妮好戊\t96\nni'hao\t妮好己\t95\n"
              "ni'hao\t妮好庚\t94\nni'hao\t妮好辛\t93\n"
              "ni'hao\t妮好壬\t92\nni'hao\t妮好癸\t91\n");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = dictionaryPath.string();
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("nihao"), "longer input is accepted");
    assertTrue(countSource(provider.page(),
                           modernime::core::CandidateSource::UserDictionary) <=
                   8,
               "longer input keeps at most eight manual candidates");
    std::error_code error;
    std::filesystem::remove(dictionaryPath, error);
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

} // namespace

int main() {
    testUserDictionarySkipsBadRowsAndKeepsLastDuplicate();
    testProfessionalPhraseAppearsFromConfiguredDictionary();
    testManualCandidatesAreCappedForShortInput();
    testManualCandidatesExpandForLongerInput();
    return EXIT_SUCCESS;
}
