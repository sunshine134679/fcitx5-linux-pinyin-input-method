#include "modernime/core/learning_store.h"
#include "modernime/pinyin/user_dictionary.h"
#include "modernime/settings/data_controller.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void assertTrue(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "data controller test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path testDirectory(std::string_view name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("modernime-data-controller-" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    assertTrue(!error, "test directory is created");
    return path;
}

void testUserDictionaryEditingAndRoundTrip() {
    modernime::pinyin::UserDictionary dictionary;
    assertTrue(dictionary.upsert("NI'HAO", "你好", 10.0F),
               "valid dictionary entry is added");
    assertTrue(dictionary.upsert("nihao", "你好", 25.0F),
               "normalized duplicate is replaced");
    assertTrue(dictionary.entries().size() == 1 &&
                   dictionary.entries().front().pinyin == "nihao" &&
                   dictionary.entries().front().weight == 25.0F,
               "duplicate replacement keeps one normalized entry");
    assertTrue(!dictionary.upsert("ni!hao", "非法拼音", 1.0F),
               "invalid pinyin is rejected");
    std::string invalidPhrase(1, static_cast<char>(0xff));
    assertTrue(!dictionary.upsert("ni", invalidPhrase, 1.0F),
               "invalid UTF-8 is rejected");
    assertTrue(!dictionary.upsert("ni", "负权重", -1.0F),
               "negative weight is rejected");

    const auto directory = testDirectory("dictionary");
    const auto path = directory / "user-dictionary.txt";
    std::string error;
    assertTrue(modernime::settings::DataController::saveDictionary(
                   path, dictionary.entries(), &error),
               "dictionary is saved: " + error);
    const auto loaded = modernime::settings::DataController::loadDictionary(path);
    assertTrue(loaded.size() == 1 && loaded.front().phrase == "你好" &&
                   loaded.front().weight == 25.0F,
               "saved dictionary loads back");

    auto invalidEntries = loaded;
    invalidEntries.front().pinyin = "bad!pinyin";
    error.clear();
    assertTrue(!modernime::settings::DataController::saveDictionary(
                   directory / "rejected.txt", invalidEntries, &error) &&
                   !error.empty(),
               "invalid dictionary export is rejected");
    assertTrue(!std::filesystem::exists(directory / "rejected.txt"),
               "rejected export does not create a file");
}

void testDictionaryImportExportUsesValidatedRows() {
    const auto directory = testDirectory("import");
    const auto source = directory / "source.txt";
    const auto target = directory / "target.txt";
    {
        std::ofstream output(source);
        output << "# comment\nni'hao\t你好\t100\n"
               << "bad!pinyin\t不应导入\t100\n";
    }
    const auto imported = modernime::settings::DataController::loadDictionary(source);
    assertTrue(imported.size() == 1 && imported.front().phrase == "你好",
               "import ignores malformed source rows");
    std::string error;
    assertTrue(modernime::settings::DataController::saveDictionary(
                   target, imported, &error),
               "valid imported rows are exported: " + error);
    assertTrue(modernime::settings::DataController::loadDictionary(target).size() ==
                   1,
               "exported import round-trips");
}

void testDictionaryImportIsStrictAndNonDestructive() {
    const auto directory = testDirectory("strict-import");
    const auto source = directory / "source.txt";
    const auto validSource = directory / "valid-source.txt";
    const auto target = directory / "target.txt";
    std::string error;
    assertTrue(modernime::settings::DataController::saveDictionary(
                   target,
                   {modernime::pinyin::UserDictionaryEntry{"ni", "原词条",
                                                          10.0F}},
                   &error),
               "strict import target fixture is saved: " + error);

    {
        std::ofstream output(source);
        output << "ni\t合法词\t80\n"
               << "bad!pinyin\t非法词\t100\n";
    }
    std::vector<modernime::pinyin::UserDictionaryEntry> imported;
    assertTrue(!modernime::settings::DataController::importDictionary(
                   source, imported, &error) && !error.empty(),
               "invalid import is rejected with a diagnostic");
    assertTrue(modernime::settings::DataController::loadDictionary(target)
                   .front()
                   .phrase == "原词条",
               "rejected import does not alter the existing dictionary");

    {
        std::ofstream output(validSource);
        output << "# comment\nni'hao\t你好\t100\n"
               << "rengongzhineng\t人工智能\t120\n";
    }
    error.clear();
    assertTrue(modernime::settings::DataController::importDictionary(
                   validSource, imported, &error) && imported.size() == 2 &&
                   error.empty(),
               "valid import returns all normalized entries");
}

void testLearningBackupThenClear() {
    const auto directory = testDirectory("learning");
    const auto databasePath = directory / "learning.sqlite3";
    const auto backupPath = directory / "learning-backup.sqlite3";
    modernime::core::LearningStore seed(databasePath);
    assertTrue(seed.open(), "learning database opens for seeding");
    assertTrue(seed.recordSelection("人工智能", "rengongzhineng", {}, {}, 1000),
               "learning record is seeded");
    seed.close();

    std::string error;
    assertTrue(modernime::settings::DataController::learningEntryCount(
                   databasePath, &error) == 1,
               "learning entry count is available to the settings client");
    assertTrue(modernime::settings::DataController::backupAndClearLearning(
                   databasePath, backupPath, &error),
               "learning backup and clear succeeds: " + error);

    modernime::core::LearningStore cleared(databasePath);
    assertTrue(cleared.open() && cleared.snapshot()->entries().empty(),
               "cleared learning database is empty");
    cleared.close();
    assertTrue(modernime::settings::DataController::learningEntryCount(
                   databasePath, &error) == 0,
               "learning entry count updates after clearing");
    modernime::core::LearningStore backup(backupPath);
    assertTrue(backup.open() && backup.snapshot()->entry(
                                   "人工智能", "rengongzhineng", {}, {}) != nullptr,
               "backup remains readable and preserves learning data");
    backup.close();

    const auto missingPath = directory / "not-created.sqlite3";
    assertTrue(modernime::settings::DataController::learningEntryCount(
                   missingPath, &error) == 0 && error.empty() &&
                   !std::filesystem::exists(missingPath),
               "missing learning database is shown as empty without creation");
}

void testFailedBackupDoesNotClearLearning() {
    const auto directory = testDirectory("failed-backup");
    const auto databasePath = directory / "learning.sqlite3";
    const auto backupDirectory = directory / "backup-directory";
    std::error_code errorCode;
    std::filesystem::create_directory(backupDirectory, errorCode);
    modernime::core::LearningStore seed(databasePath);
    assertTrue(seed.open(), "failure test database opens");
    assertTrue(seed.recordSelection("保留词", "baoliuci", {}, {}, 1000),
               "failure test learning record is seeded");
    seed.close();

    std::string error;
    assertTrue(!modernime::settings::DataController::backupAndClearLearning(
                   databasePath, backupDirectory, &error) && !error.empty(),
               "failed backup is reported");
    modernime::core::LearningStore untouched(databasePath);
    assertTrue(untouched.open() && untouched.snapshot()->entry(
                                      "保留词", "baoliuci", {}, {}) != nullptr,
               "failed backup leaves original learning data untouched");
    untouched.close();
}

} // namespace

int main() {
    testUserDictionaryEditingAndRoundTrip();
    testDictionaryImportExportUsesValidatedRows();
    testDictionaryImportIsStrictAndNonDestructive();
    testLearningBackupThenClear();
    testFailedBackupDoesNotClearLearning();
    return EXIT_SUCCESS;
}
