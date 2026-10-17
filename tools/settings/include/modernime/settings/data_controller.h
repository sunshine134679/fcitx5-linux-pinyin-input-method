#pragma once

#include "modernime/pinyin/user_dictionary.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::settings {

enum class DictionaryConflictPolicy {
    KeepExisting,
    OverwriteWithImport,
};

enum class LearningBackendKind {
    Unknown,
    Sqlite,
    Lmdb,
};

struct LearningBackendInfo final {
    LearningBackendKind kind = LearningBackendKind::Unknown;
    std::string name;
    std::string description;
};

struct DictionaryImportPreview final {
    bool valid = false;
    std::string error;
    std::size_t totalParsed = 0;
    std::size_t newEntries = 0;
    std::size_t duplicateEntries = 0;
    std::size_t conflictEntries = 0;
    std::size_t invalidLines = 0;
    std::vector<pinyin::UserDictionaryEntry> parsedEntries;
};

class DataController final {
public:
    static std::vector<pinyin::UserDictionaryEntry> loadDictionary(
        const std::filesystem::path &path);
    static std::vector<std::size_t> filterDictionary(
        std::span<const pinyin::UserDictionaryEntry> entries,
        std::string_view query);
    static bool importDictionary(
        const std::filesystem::path &path,
        std::vector<pinyin::UserDictionaryEntry> &entries,
        std::string *error = nullptr);
    static DictionaryImportPreview previewDictionaryImport(
        const std::filesystem::path &path,
        const std::vector<pinyin::UserDictionaryEntry> &currentEntries);
    static std::vector<pinyin::UserDictionaryEntry> mergeDictionary(
        const std::vector<pinyin::UserDictionaryEntry> &current,
        const std::vector<pinyin::UserDictionaryEntry> &imported,
        DictionaryConflictPolicy policy);
    static bool createDictionaryBackup(
        const std::filesystem::path &path,
        std::filesystem::path *backupPathOut = nullptr,
        std::string *error = nullptr);
    static bool saveDictionary(
        const std::filesystem::path &path,
        const std::vector<pinyin::UserDictionaryEntry> &entries,
        std::string *error = nullptr);
    static std::size_t learningEntryCount(const std::filesystem::path &path,
                                          std::string *error = nullptr);
    static LearningBackendInfo detectActiveLearningBackend(
        const std::filesystem::path &path);
    static bool backupAndClearLearning(const std::filesystem::path &path,
                                       const std::filesystem::path &backupPath,
                                       std::string *error = nullptr);

    using IoHook = std::function<void(std::string_view op)>;
    static void setIoHookForTesting(IoHook hook);
};

} // namespace modernime::settings
