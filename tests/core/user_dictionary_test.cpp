#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/user_dictionary.h"

#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinencoder.h>

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

void testMalformedSyllablesDoNotAbortDictionaryLoad() {
    modernime::pinyin::UserDictionary dictionary;
    assertTrue(dictionary.upsert("zzzz", "错误", 100), "malformed row accepted for validation");
    assertTrue(dictionary.upsert("abc", "错误", 100), "partial syllable row accepted for validation");
    assertTrue(dictionary.upsert("nihao", "你好", 100), "valid row follows malformed rows");
    libime::PinyinDictionary native;
    try {
        dictionary.addTo(native, 1);
    } catch (const std::exception &) {
        assertTrue(false, "malformed syllables cannot abort user dictionary loading");
    }
    const auto encoded = libime::PinyinEncoder::encodeFullPinyin("ni'hao");
    bool found = false;
    native.matchWords(encoded.data(), encoded.size(),
                      [&](std::string_view, std::string_view phrase, float) {
                          found = found || phrase == "你好";
                          return true;
                      });
    assertTrue(found, "valid rows remain usable after malformed syllables");
}

void testAmbiguousPinyinLoadHasBoundedWork() {
    modernime::pinyin::UserDictionary dictionary;
    std::string pinyin;
    std::string phrase;
    for (int repeat = 0; repeat < 24; ++repeat) {
        pinyin += "changan";
        phrase += "长安";
    }
    assertTrue(dictionary.upsert(pinyin, phrase, 100), "ambiguous dictionary entry accepted");
    libime::PinyinDictionary native;
    const auto start = std::chrono::steady_clock::now();
    dictionary.addTo(native, 1);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assertTrue(elapsed < std::chrono::seconds(1),
               "ambiguous pinyin loads in under one second without enumerating every path");
}

void testPinyinSegmentationPreservesExistingChoice() {
    for (const auto pinyin : {"nihao", "changan", "tiananmen", "rengongzhineng"}) {
        auto graph = libime::PinyinEncoder::parseUserPinyin(
            pinyin, libime::PinyinFuzzyFlag::None);
        std::vector<std::size_t> best;
        graph.dfs([&](const auto &, const auto &path) {
            if (path.size() > best.size()) { best = path; }
            return true;
        });
        std::string segmented;
        std::size_t previous = 0;
        for (const auto end : best) {
            if (!segmented.empty()) { segmented += '\''; }
            segmented.append(pinyin + previous, end - previous);
            previous = end;
        }
        modernime::pinyin::UserDictionary dictionary;
        dictionary.upsert(pinyin, "测试", 100);
        libime::PinyinDictionary native;
        dictionary.addTo(native, 1);
        const auto encoded = libime::PinyinEncoder::encodeFullPinyin(segmented);
        bool found = false;
        native.matchWords(encoded.data(), encoded.size(),
                          [&](std::string_view, std::string_view phrase, float) {
                              found = found || phrase == "测试";
                              return true;
                          });
        assertTrue(found, "segmentation keeps the first deepest DFS choice for common phrases");
    }
}

void testUserDictionaryLoadSnapshotDiagnostics() {
    const auto nonExistent = testPath("non-existent-dict.txt");
    const auto snapNonExistent = modernime::pinyin::UserDictionary::loadSnapshot(nonExistent);
    assertTrue(!snapNonExistent.exists, "non-existent file reports exists=false");
    assertTrue(!snapNonExistent.readable, "non-existent file reports readable=false");
    assertTrue(snapNonExistent.contentHash.empty(), "non-existent content hash is empty");
    assertTrue(snapNonExistent.entries.empty(), "non-existent entries empty");

    const auto dirPath = testPath("test-dir-dict");
    std::filesystem::create_directories(dirPath);
    const auto snapDir = modernime::pinyin::UserDictionary::loadSnapshot(dirPath);
    assertTrue(snapDir.exists, "dir reports exists=true");
    assertTrue(!snapDir.readable, "dir reports readable=false");
    assertTrue(!snapDir.error.empty(), "dir reports error");
    std::filesystem::remove_all(dirPath);

    const auto corrupted = testPath("corrupted-dict.txt");
    writeFile(corrupted,
              "# comment\n"
              "ni'hao\t你好\t100\n"
              "broken_columns\n"
              "bad!pinyin\t词条\t50\n"
              "zhong'wen\t中文\tnot_a_weight\n"
              "wan'mei\t完美\t80\n");
    const auto snapCorrupted = modernime::pinyin::UserDictionary::loadSnapshot(corrupted);
    assertTrue(snapCorrupted.exists && snapCorrupted.readable, "file exists and is readable");
    assertTrue(!snapCorrupted.contentHash.empty(), "content hash is computed");
    assertTrue(snapCorrupted.entries.size() == 2, "2 valid entries are kept");
    assertTrue(snapCorrupted.entries[0].phrase == "你好" && snapCorrupted.entries[1].phrase == "完美",
               "correct entries kept");
    assertTrue(snapCorrupted.diagnostics.size() == 3, "3 corrupted lines diagnosed");
    assertTrue(snapCorrupted.diagnostics[0].lineNumber == 3, "diagnostic line 3");
    assertTrue(snapCorrupted.diagnostics[1].lineNumber == 4, "diagnostic line 4");
    assertTrue(snapCorrupted.diagnostics[2].lineNumber == 5, "diagnostic line 5");
    std::filesystem::remove(corrupted);
}

} // namespace

int main() {
    testAmbiguousPinyinLoadHasBoundedWork();
    testMalformedSyllablesDoNotAbortDictionaryLoad();
    testPinyinSegmentationPreservesExistingChoice();
    testUserDictionarySkipsBadRowsAndKeepsLastDuplicate();
    testUserDictionaryRejectsTrailingWeightData();
    testUserDictionaryRejectsMalformedPinyin();
    testUserDictionaryRejectsInvalidUtf8Phrase();
    testUserDictionaryAcceptsWindowsLineEndings();
    testProfessionalPhraseAppearsFromConfiguredDictionary();
    testManualCandidatesAreCappedForShortInput();
    testManualCandidatesExpandForLongerInput();
    testUserDictionaryFastContainsWithLargeCorpus();
    testUserDictionaryLoadSnapshotDiagnostics();
    return EXIT_SUCCESS;
}
