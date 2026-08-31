#include "modernime/core/persistent_clipboard_history.h"

#include <chrono>
#include <system_error>
#include <utility>

namespace modernime::core {
namespace {

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::filesystem::file_time_type currentMtime(const std::filesystem::path &path,
                                             bool &valid) {
    std::error_code code;
    const auto mtime = std::filesystem::last_write_time(path, code);
    valid = !code;
    return mtime;
}

} // namespace

PersistentClipboardHistory::PersistentClipboardHistory(
    std::filesystem::path path)
    : path_(std::move(path)) {}

bool PersistentClipboardHistory::quarantineCorruptFile() const {
    std::error_code filesystemError;
    if (!std::filesystem::exists(path_, filesystemError) || filesystemError) {
        return false;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    auto backup = path_;
    backup += ".corrupt-";
    backup += std::to_string(stamp);
    std::filesystem::rename(path_, backup, filesystemError);
    return !filesystemError;
}

bool PersistentClipboardHistory::load(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    const bool result = history_.load(path_, error);
    if (!result && quarantineCorruptFile()) {
        // 损坏或不可读的文件已改名备份；剪贴板以空历史继续运行，
        // 而不是永久失效直到用户手动删除。
        loaded_ = true;
        if (error != nullptr) {
            error->clear();
        }
    } else {
        loaded_ = result;
    }
    dirty_ = false;
    bool mtimeValid = false;
    const auto mtime = currentMtime(path_, mtimeValid);
    knownMtime_ = mtime;
    knownMtimeValid_ = mtimeValid;
    return loaded_;
}

bool PersistentClipboardHistory::observe(std::string_view text,
                                         std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!loaded_) {
        setError(error, "clipboard history is not loaded");
        return false;
    }

    const auto changed = history_.observe(text);
    if (changed) {
        dirty_ = true;
    }
    if (!dirty_) {
        return changed;
    }

    if (!persist(error)) {
        return changed;
    }
    return changed;
}

bool PersistentClipboardHistory::reloadIfExternallyChanged(
    std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (dirty_) {
        return false;
    }
    bool mtimeValid = false;
    const auto mtime = currentMtime(path_, mtimeValid);
    if (!mtimeValid) {
        return false;
    }
    if (knownMtimeValid_ && mtime == knownMtime_) {
        return false;
    }
    knownMtime_ = mtime;
    knownMtimeValid_ = true;
    const bool result = history_.load(path_, error);
    if (!result && quarantineCorruptFile()) {
        // 外部修改把文件写坏时同样隔离备份并继续运行。
        loaded_ = true;
        if (error != nullptr) {
            error->clear();
        }
    } else {
        loaded_ = result;
    }
    dirty_ = false;
    return loaded_;
}

bool PersistentClipboardHistory::flush(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (!loaded_) {
        setError(error, "clipboard history is not loaded");
        return false;
    }
    return persist(error);
}

bool PersistentClipboardHistory::persist(std::string *error) {
    if (!history_.save(path_, error)) {
        return false;
    }
    dirty_ = false;
    bool mtimeValid = false;
    knownMtime_ = currentMtime(path_, mtimeValid);
    knownMtimeValid_ = mtimeValid;
    return true;
}

} // namespace modernime::core
