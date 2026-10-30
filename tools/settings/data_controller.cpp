#include "modernime/settings/data_controller.h"

#include "modernime/core/learning_store.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>
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

std::string foldAscii(std::string_view value) {
    std::string result(value);
    for (auto &character : result) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return result;
}

std::mutex s_hookMutex;
DataController::IoHook s_ioHook;

void notifyIoHook(std::string_view op) {
    DataController::IoHook localHook;
    {
        std::lock_guard<std::mutex> lock(s_hookMutex);
        if (!s_ioHook) {
            return;
        }
        localHook = s_ioHook;
    }
    localHook(op);
}

} // namespace

std::vector<std::size_t> DataController::filterDictionary(
    std::span<const pinyin::UserDictionaryEntry> entries,
    std::string_view query) {
    std::vector<std::size_t> indices;
    if (query.empty()) {
        indices.resize(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            indices[i] = i;
        }
        return indices;
    }

    const auto foldedQuery = foldAscii(query);
    indices.reserve(std::min<std::size_t>(entries.size(), 1024));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &entry = entries[i];
        const auto foldedPinyin = foldAscii(entry.pinyin);
        const auto foldedPhrase = foldAscii(entry.phrase);
        if (foldedPinyin.find(foldedQuery) != std::string::npos ||
            foldedPhrase.find(foldedQuery) != std::string::npos) {
            indices.push_back(i);
        }
    }
    return indices;
}

DictionarySnapshot DataController::loadDictionaryForEditing(
    const std::filesystem::path &path) {
    notifyIoHook("loadDictionaryForEditing");
    notifyIoHook("loadDictionary");
    const auto raw = pinyin::UserDictionary::loadSnapshot(path);
    DictionarySnapshot snapshot;
    snapshot.exists = raw.exists;
    snapshot.contentHash = raw.contentHash;
    snapshot.entries = raw.entries;
    snapshot.error = raw.error;
    for (const auto &diag : raw.diagnostics) {
        snapshot.diagnostics.push_back({diag.lineNumber, diag.lineContent, diag.reason});
    }

    if (!raw.exists) {
        snapshot.status = DictionaryLoadStatus::NonExistent;
    } else if (!raw.readable) {
        snapshot.status = DictionaryLoadStatus::IoError;
    } else if (!snapshot.diagnostics.empty()) {
        snapshot.status = DictionaryLoadStatus::CorruptedLines;
    } else {
        snapshot.status = DictionaryLoadStatus::Success;
    }
    return snapshot;
}

std::vector<pinyin::UserDictionaryEntry> DataController::loadDictionary(
    const std::filesystem::path &path) {
    return loadDictionaryForEditing(path).entries;
}

float DataController::normalizeImportedWeight(float rawWeight) noexcept {
    if (!std::isfinite(rawWeight) || rawWeight <= 0.0F) {
        return 0.0F;
    }
    const float smoothed = std::round(10.0F * std::log(1.0F + rawWeight));
    return std::min(50.0F, smoothed);
}

DictionaryImportPreview DataController::previewDictionaryImport(
    const std::filesystem::path &path,
    const std::vector<pinyin::UserDictionaryEntry> &currentEntries) {
    notifyIoHook("previewDictionaryImport");
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
            const auto rawWeight = std::stof(weightText, &consumed);
            if (consumed != weightText.size() || !std::isfinite(rawWeight) ||
                rawWeight < 0.0F) {
                ++preview.invalidLines;
                if (preview.error.empty()) {
                    preview.error = "用户词典第 " + std::to_string(lineNumber) +
                                    " 行权重无效";
                }
                continue;
            }

            const auto weight = normalizeImportedWeight(rawWeight);

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
    const auto originalSnapshot = pinyin::UserDictionary::loadSnapshot(path);
    if (!originalSnapshot.readable) {
        setError(error, "无法读取原词典文件进行备份");
        return false;
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
    const auto backupSnapshot = pinyin::UserDictionary::loadSnapshot(backupPath);
    if (!backupSnapshot.readable || backupSnapshot.contentHash != originalSnapshot.contentHash) {
        setError(error, "词典备份内容校验未通过（内容哈希不一致）");
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

DictionarySaveResult DataController::saveDictionaryIfUnchanged(
    const std::filesystem::path &path,
    const std::vector<pinyin::UserDictionaryEntry> &entries,
    const DictionaryExpectedBaseline &expectedBaseline,
    std::string *error) {
    notifyIoHook("saveDictionaryIfUnchanged");
    notifyIoHook("saveDictionary");
    const auto setErr = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
    };
    if (error != nullptr) {
        error->clear();
    }
    if (path.empty()) {
        setErr("用户词典路径为空");
        return DictionarySaveResult::Invalid;
    }

    // Advisory lock using flock on .lock file
    const auto lockPath = path.string() + ".lock";
    int lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockFd < 0) {
        setErr("无法获取词典锁文件: " + std::string(strerror(errno)));
        return DictionarySaveResult::IoError;
    }
    struct LockGuard {
        int fd = -1;
        ~LockGuard() {
            if (fd >= 0) {
                flock(fd, LOCK_UN);
                close(fd);
            }
        }
    } guard;
    if (flock(lockFd, LOCK_EX) != 0) {
        close(lockFd);
        setErr("无法锁定词典文件: " + std::string(strerror(errno)));
        return DictionarySaveResult::IoError;
    }
    guard.fd = lockFd;

    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);

    if (expectedBaseline.kind == DictionaryExpectedBaseline::Kind::ExpectNonExistent) {
        if (exists) {
            setErr("词典保存冲突：文件已被外部创建");
            return DictionarySaveResult::Conflict;
        }
    } else if (expectedBaseline.kind == DictionaryExpectedBaseline::Kind::ExpectHash) {
        if (!exists) {
            setErr("词典保存冲突：原词典文件已被外部删除");
            return DictionarySaveResult::Conflict;
        }
        const auto currentSnapshot = pinyin::UserDictionary::loadSnapshot(path);
        if (!currentSnapshot.readable) {
            setErr("无法读取现有词典进行冲突校验");
            return DictionarySaveResult::IoError;
        }
        if (currentSnapshot.contentHash != expectedBaseline.contentHash) {
            setErr("词典保存冲突：文件内容已被外部修改");
            return DictionarySaveResult::Conflict;
        }
    }

    pinyin::UserDictionary dictionary;
    for (const auto &entry : entries) {
        if (!dictionary.upsert(entry.pinyin, entry.phrase, entry.weight)) {
            setErr("用户词典包含无效词条");
            return DictionarySaveResult::Invalid;
        }
    }
    if (!dictionary.saveText(path)) {
        setErr("无法保存用户词典");
        return DictionarySaveResult::IoError;
    }
    return DictionarySaveResult::Saved;
}

bool DataController::saveDictionary(
    const std::filesystem::path &path,
    const std::vector<pinyin::UserDictionaryEntry> &entries,
    std::string *error) {
    return saveDictionaryIfUnchanged(path, entries,
                                    DictionaryExpectedBaseline::unchecked(),
                                    error) == DictionarySaveResult::Saved;
}

std::size_t DataController::learningEntryCount(
    const std::filesystem::path &path, std::string *error) {
    notifyIoHook("learningEntryCount");
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
    const auto count = store.entryCount();
    store.close();
    if (!count.has_value()) {
        if (error != nullptr) {
            *error = "无法读取学习数据库条目数";
        }
        return 0;
    }
    notifyIoHook("learningEntryCount:afterCount");
    return *count;
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
    notifyIoHook("backupAndClearLearning");
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
    notifyIoHook("backupAndClearLearning:afterBackup");

    // Self-verify the backup itself rather than comparing with the continuing live store.
    core::LearningStore verification(backupPath);
    if (!verification.openReadOnly() || !verification.verifySelf()) {
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

void DataController::setIoHookForTesting(IoHook hook) {
    std::lock_guard<std::mutex> lock(s_hookMutex);
    s_ioHook = std::move(hook);
}

} // namespace modernime::settings
