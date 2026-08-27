#include "modernime/settings/data_controller.h"

#include "modernime/core/learning_store.h"

#include <utility>

namespace modernime::settings {

std::vector<pinyin::UserDictionaryEntry> DataController::loadDictionary(
    const std::filesystem::path &path) {
    return pinyin::UserDictionary::loadText(path).entries();
}

bool DataController::saveDictionary(
    const std::filesystem::path &path,
    const std::vector<pinyin::UserDictionaryEntry> &entries,
    std::string *error) {
    const auto setError = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    };
    pinyin::UserDictionary dictionary;
    for (const auto &entry : entries) {
        if (!dictionary.upsert(entry.pinyin, entry.phrase, entry.weight)) {
            setError("user dictionary contains an invalid entry");
            return false;
        }
    }
    if (!dictionary.saveText(path)) {
        setError("unable to save user dictionary");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool DataController::backupAndClearLearning(
    const std::filesystem::path &path, const std::filesystem::path &backupPath,
    std::string *error) {
    const auto setError = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    };
    if (path.empty() || backupPath.empty() || path == backupPath) {
        setError("learning or backup path is invalid");
        return false;
    }
    core::LearningStore store(path);
    if (!store.open()) {
        setError("unable to open learning database");
        return false;
    }
    if (!store.backupTo(backupPath)) {
        setError("unable to create learning backup");
        return false;
    }

    core::LearningStore verification(backupPath);
    if (!verification.open() || verification.snapshot() == nullptr) {
        setError("learning backup could not be verified");
        return false;
    }
    verification.close();
    if (!store.clear()) {
        setError("unable to clear learning database");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace modernime::settings
