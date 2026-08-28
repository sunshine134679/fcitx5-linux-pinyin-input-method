#pragma once

#include "modernime/pinyin/user_dictionary.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace modernime::settings {

class DataController final {
public:
    static std::vector<pinyin::UserDictionaryEntry> loadDictionary(
        const std::filesystem::path &path);
    static bool saveDictionary(
        const std::filesystem::path &path,
        const std::vector<pinyin::UserDictionaryEntry> &entries,
        std::string *error = nullptr);
    static std::size_t learningEntryCount(const std::filesystem::path &path,
                                          std::string *error = nullptr);
    static bool backupAndClearLearning(const std::filesystem::path &path,
                                       const std::filesystem::path &backupPath,
                                       std::string *error = nullptr);
};

} // namespace modernime::settings
