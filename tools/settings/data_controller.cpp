#include "modernime/settings/data_controller.h"

#include "modernime/core/learning_store.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace modernime::settings {

namespace {

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string keyFor(std::string_view pinyin, std::string_view phrase) {
    std::string key(pinyin);
    key.push_back('\x1f');
    key.append(phrase);
    return key;
}

} // namespace

std::vector<pinyin::UserDictionaryEntry> DataController::loadDictionary(
    const std::filesystem::path &path) {
    return pinyin::UserDictionary::loadText(path).entries();
}

DictionaryImportPreview DataController::previewDictionaryImport(
    const std::filesystem::path &path,
    const std::vector<pinyin::UserDictionaryEntry> &currentEntries) {
    DictionaryImportPreview preview;
    if (path.empty()) {
        preview.error = "用户词典导入路径为空";
        return preview;
    }

    std::ifstream input(path);
    if (!input) {
        preview.error = "无法打开要导入的用户词典";
        return preview;
    }

    std::string line;
    std::size_t lineNumber = 0;
    std::unordered_map<std::string, std::size_t> importIndex;

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
            ++preview.invalidLines;
            if (preview.error.empty()) {
                preview.error = "用户词典第 " + std::to_string(lineNumber) +
                                " 行格式无效，需要拼音、词条和权重三列";
            }
            continue;
        }

        try {
            std::size_t consumed = 0;
            const auto weight = std::stof(weightText, &consumed);
            if (consumed != weightText.size() || !std::isfinite(weight) ||
                weight < 0.0F) {
                ++preview.invalidLines;
                if (preview.error.empty()) {
                    preview.error = "用户词典第 " + std::to_string(lineNumber) +
                                    " 行权重无效";
                }
                continue;
            }

            pinyin::UserDictionary rowChecker;
            if (!rowChecker.upsert(pinyin, phrase, weight)) {
                ++preview.invalidLines;
                if (preview.error.empty()) {
                    preview.error = "用户词典第 " + std::to_string(lineNumber) +
                                    " 行内容无效";
                }
                continue;
            }

            const auto normalizedEntry = rowChecker.entries().front();
            const auto key = keyFor(normalizedEntry.pinyin, normalizedEntry.phrase);
            const auto [it, inserted] = importIndex.emplace(key, preview.parsedEntries.size());
            if (inserted) {
                preview.parsedEntries.push_back(normalizedEntry);
            } else {
                preview.parsedEntries[it->second] = normalizedEntry;
            }
            ++preview.totalParsed;
        } catch (...) {
            ++preview.invalidLines;
            if (preview.error.empty()) {
                preview.error = "用户词典第 " + std::to_string(lineNumber) +
                                " 行权重无效";
            }
            continue;
        }
    }

    if (input.bad()) {
        preview.error = "读取用户词典时发生 I/O 错误";
        return preview;
    }

    if (preview.invalidLines > 0) {
        preview.valid = false;
        return preview;
    }

    if (preview.parsedEntries.empty()) {
        preview.error = "导入文件中没有有效词条（需拼音、词条、权重三列），已取消导入";
        return preview;
    }

    std::unordered_map<std::string, float> currentMap;
    for (const auto &entry : currentEntries) {
        currentMap[keyFor(entry.pinyin, entry.phrase)] = entry.weight;
    }

    for (const auto &entry : preview.parsedEntries) {
        const auto key = keyFor(entry.pinyin, entry.phrase);
        const auto it = currentMap.find(key);
        if (it == currentMap.end()) {
            ++preview.newEntries;
        } else {
            if (std::fabs(it->second - entry.weight) < 1e-4F) {
                ++preview.duplicateEntries;
            } else {
                ++preview.conflictEntries;
            }
        }
    }

    preview.valid = true;
    preview.error.clear();
    return preview;
}

bool DataController::importDictionary(
    const std::filesystem::path &path,
    std::vector<pinyin::UserDictionaryEntry> &entries, std::string *error) {
    const auto preview = previewDictionaryImport(path, {});
    if (!preview.valid) {
        setError(error, preview.error.empty() ? "导入文件中没有有效词条" : preview.error);
        return false;
    }
    entries = preview.parsedEntries;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::vector<pinyin::UserDictionaryEntry> DataController::mergeDictionary(
    const std::vector<pinyin::UserDictionaryEntry> &current,
    const std::vector<pinyin::UserDictionaryEntry> &imported,
    DictionaryConflictPolicy policy) {
    auto result = current;
    std::unordered_map<std::string, std::size_t> indexMap;
    for (std::size_t i = 0; i < result.size(); ++i) {
        indexMap[keyFor(result[i].pinyin, result[i].phrase)] = i;
    }
    for (const auto &entry : imported) {
        const auto key = keyFor(entry.pinyin, entry.phrase);
        const auto it = indexMap.find(key);
        if (it == indexMap.end()) {
            indexMap[key] = result.size();
            result.push_back(entry);
        } else {
            if (policy == DictionaryConflictPolicy::OverwriteWithImport) {
                result[it->second].weight = entry.weight;
            }
        }
    }
    return result;
}

bool DataController::createDictionaryBackup(
    const std::filesystem::path &path,
    std::filesystem::path *backupPathOut,
    std::string *error) {
    if (path.empty()) {
        setError(error, "用户词典备份路径为空");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    const auto backupPath = path.string() + ".bak";
    if (backupPathOut != nullptr) {
        *backupPathOut = backupPath;
    }
    std::filesystem::copy_file(path, backupPath,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        setError(error, "无法创建词典备份文件：" + ec.message());
        return false;
    }
    if (!std::filesystem::exists(backupPath, ec)) {
        setError(error, "词典备份文件写入验证失败");
        return false;
    }
    const auto originalEntries = loadDictionary(path);
    const auto backupEntries = loadDictionary(backupPath);
    if (originalEntries.size() != backupEntries.size()) {
        setError(error, "词典备份内容校验未通过（条目数不一致）");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
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
    if (!store.openReadOnly()) {
        if (error != nullptr) {
            *error = "无法打开学习数据库";
        }
        return 0;
    }
    const auto snapshot = store.snapshot();
    store.close();
    if (snapshot == nullptr) {
        if (error != nullptr) {
            *error = "无法读取学习数据库快照";
        }
        return 0;
    }
    return snapshot->entries().size();
}

LearningBackendInfo DataController::detectActiveLearningBackend(
    const std::filesystem::path &path) {
    core::LearningStore store(path);
    if (store.isLmdbBackend()) {
        return {LearningBackendKind::Lmdb, "LMDB",
                "高性能内存映射引擎 (Zero-Copy MMap)"};
    }
    return {LearningBackendKind::Sqlite, "SQLite WAL",
            "嵌入式关系型存储 (WAL 并发保护)"};
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
    if (!verification.openReadOnly() || verification.snapshot() == nullptr) {
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
