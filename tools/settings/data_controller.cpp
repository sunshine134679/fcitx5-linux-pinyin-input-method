#include "modernime/settings/data_controller.h"

#include "modernime/core/learning_store.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

namespace modernime::settings {

namespace {

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

std::vector<pinyin::UserDictionaryEntry> DataController::loadDictionary(
    const std::filesystem::path &path) {
    return pinyin::UserDictionary::loadText(path).entries();
}

bool DataController::importDictionary(
    const std::filesystem::path &path,
    std::vector<pinyin::UserDictionaryEntry> &entries, std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setError(error, "用户词典导入路径为空");
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        setError(error, "无法打开要导入的用户词典");
        return false;
    }

    pinyin::UserDictionary dictionary;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::string pinyin;
        std::string phrase;
        std::string weightText;
        std::string extra;
        std::istringstream fields(line);
        if (!std::getline(fields, pinyin, '\t') ||
            !std::getline(fields, phrase, '\t') ||
            !std::getline(fields, weightText, '\t') ||
            std::getline(fields, extra, '\t')) {
            setError(error, "用户词典第 " + std::to_string(lineNumber) +
                               " 行格式无效，需要拼音、词条和权重三列");
            return false;
        }

        try {
            std::size_t consumed = 0;
            const auto weight = std::stof(weightText, &consumed);
            if (consumed != weightText.size() || !std::isfinite(weight) ||
                weight < 0.0F || !dictionary.upsert(pinyin, phrase, weight)) {
                setError(error, "用户词典第 " + std::to_string(lineNumber) +
                                   " 行内容无效");
                return false;
            }
        } catch (...) {
            setError(error, "用户词典第 " + std::to_string(lineNumber) +
                               " 行权重无效");
            return false;
        }
    }
    if (input.bad()) {
        setError(error, "读取用户词典时发生 I/O 错误");
        return false;
    }

    entries = dictionary.entries();
    return true;
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
            setError("用户词典包含无效词条");
            return false;
        }
    }
    if (!dictionary.saveText(path)) {
        setError("无法保存用户词典");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::size_t DataController::learningEntryCount(
    const std::filesystem::path &path, std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        if (error != nullptr) {
            *error = "学习数据库路径为空";
        }
        return 0;
    }

    std::error_code filesystemError;
    if (!std::filesystem::exists(path, filesystemError)) {
        if (filesystemError && error != nullptr) {
            *error = "无法检查学习数据库：" + filesystemError.message();
        }
        return 0;
    }

    core::LearningStore store(path);
    if (!store.open()) {
        if (error != nullptr) {
            *error = "无法打开学习数据库";
        }
        return 0;
    }
    const auto snapshot = store.snapshot();
    store.close();
    if (snapshot == nullptr) {
        if (error != nullptr) {
            *error = "无法读取学习数据库";
        }
        return 0;
    }
    return snapshot->entries().size();
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
