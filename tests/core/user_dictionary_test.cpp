#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/user_dictionary.h"

#include <chrono>
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

void testUserDictionaryRejectsTrailingWeightData() {
    const auto path = testPath("strict-weight-dictionary.txt");
    writeFile(path, "ni\t错误权重\t80abc\nni\t合法词\t80\n");
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(!dictionary.contains("ni", "错误权重"),
               "weight with trailing data is rejected");
    assertTrue(dictionary.contains("ni", "合法词"),
               "strict parsing keeps valid weight");
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testUserDictionaryRejectsMalformedPinyin() {
    const auto path = testPath("malformed-pinyin-dictionary.txt");
    const auto learningPath = testPath("malformed-pinyin-learning.sqlite3");
    writeFile(path,
              "'ni\t首撇号\t100\nni'\t尾撇号\t100\n"
              "ni''hao\t连续撇号\t100\nni!hao\t非法字符\t100\n"
              "NI'HAO\t合法大写\t100\n");
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(dictionary.entries().size() == 1 &&
                   dictionary.contains("nihao", "合法大写"),
               "only well-formed pinyin rows are loaded");
    modernime::pinyin::PinyinDataPaths paths;
    paths.userDictionary = path.string();
    paths.learningStore = learningPath.string();
    modernime::pinyin::PinyinCandidateProvider provider(paths);
    assertTrue(provider.append("nihao"),
               "malformed rows do not prevent provider startup");
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(learningPath, error);
    std::filesystem::remove(learningPath.string() + "-wal", error);
    std::filesystem::remove(learningPath.string() + "-shm", error);
}

void testUserDictionaryRejectsInvalidUtf8Phrase() {
    const auto path = testPath("invalid-utf8-dictionary.txt");
    std::string contents = "ni\t";
    contents.push_back(static_cast<char>(0xff));
    contents += "\t100\n";
    contents += "ni\t合法词\t100\n";
    writeFile(path, contents);
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(!dictionary.entries().empty() &&
                   dictionary.contains("ni", "合法词") &&
                   dictionary.entries().size() == 1,
               "invalid UTF-8 phrases are ignored without dropping valid rows");
    std::error_code error;
    std::filesystem::remove(path, error);
}

void testUserDictionaryAcceptsWindowsLineEndings() {
    const auto path = testPath("crlf-user-dictionary.txt");
    writeFile(path, "ni\t换行词\t80\r\n");
    const auto dictionary = modernime::pinyin::UserDictionary::loadText(path);
    assertTrue(dictionary.contains("ni", "换行词"),
               "CRLF dictionary rows are accepted");
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

void testUserDictionaryFastContainsWithLargeCorpus() {
    modernime::pinyin::UserDictionary dict;
    const auto makePinyin = [](int index) {
        std::string p = "ci";
        int temp = index;
        do {
            p.push_back(static_cast<char>('a' + (temp % 26)));
            temp /= 26;
        } while (temp > 0);
        return p;
    };

    for (int i = 0; i < 5000; ++i) {
        dict.upsert(makePinyin(i), "短语" + std::to_string(i), 10.0F);
    }
    assertTrue(dict.entries().size() == 5000, "all 5000 entries upserted");
    assertTrue(dict.contains(makePinyin(0), "短语0"), "contains first entry");
    assertTrue(dict.contains(makePinyin(4999), "短语4999"), "contains last entry");
    assertTrue(!dict.contains(makePinyin(5000), "短语5000"), "does not contain missing entry");

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        const int index = i % 5000;
        assertTrue(dict.contains(makePinyin(index), "短语" + std::to_string(index)),
                   "lookup succeeds");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    assertTrue(elapsed.count() < 100, "10000 contains lookups finish in < 100ms");

    assertTrue(dict.remove(makePinyin(0), "短语0"), "remove first entry succeeds");
    assertTrue(!dict.contains(makePinyin(0), "短语0"), "removed entry is no longer present");
    assertTrue(dict.entries().size() == 4999, "entries size decremented");
}

} // namespace

int main() {
    testUserDictionarySkipsBadRowsAndKeepsLastDuplicate();
    testUserDictionaryRejectsTrailingWeightData();
    testUserDictionaryRejectsMalformedPinyin();
    testUserDictionaryRejectsInvalidUtf8Phrase();
    testUserDictionaryAcceptsWindowsLineEndings();
    testProfessionalPhraseAppearsFromConfiguredDictionary();
    testManualCandidatesAreCappedForShortInput();
    testManualCandidatesExpandForLongerInput();
    testUserDictionaryFastContainsWithLargeCorpus();
    return EXIT_SUCCESS;
}
