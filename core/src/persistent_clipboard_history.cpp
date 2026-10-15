#include "modernime/core/persistent_clipboard_history.h"

#include <chrono>
#include <system_error>
#include <sys/stat.h>
#include <utility>

namespace modernime::core {
namespace {

void setError(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

PersistentClipboardHistory::PersistentClipboardHistory(
    std::filesystem::path path)
    : path_(std::move(path)) {}

PersistentClipboardHistory::FileState
PersistentClipboardHistory::currentFileState() const {
    struct stat metadata {};
    if (::stat(path_.c_str(), &metadata) != 0) {
        return {};
    }
    // save() replaces the file atomically, so device/inode detect edits even
    // when mtime and size repeat. ctime also covers in-place rewrites and inode
    // reuse between polls without reading the full clipboard on every poll.
    return {static_cast<std::uintmax_t>(metadata.st_dev),
            static_cast<std::uintmax_t>(metadata.st_ino),
            static_cast<std::uintmax_t>(metadata.st_size),
            std::chrono::seconds(metadata.st_mtim.tv_sec) +
                std::chrono::nanoseconds(metadata.st_mtim.tv_nsec),
            std::chrono::seconds(metadata.st_ctim.tv_sec) +
                std::chrono::nanoseconds(metadata.st_ctim.tv_nsec),
            true};
}

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
    pendingObservations_.clear();
    knownFileState_ = currentFileState();
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

    const bool newObservation = !hasObservedText_ || text != lastObservedText_;
    if (newObservation) {
        lastObservedText_ = text;
        hasObservedText_ = true;
    }
    const bool changed = newObservation && history_.observe(text);
    if (changed) {
        dirty_ = true;
        pendingObservations_.observe(text);
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
    const auto state = currentFileState();
    if (!state.valid) {
        return false;
    }
    if (state == knownFileState_) {
        return false;
    }
    knownFileState_ = state;
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
    return !dirty_ || persist(error);
}

bool PersistentClipboardHistory::persist(std::string *error) {
    if (currentFileState() != knownFileState_) {
        ClipboardHistory latest;
        if (!latest.load(path_, error)) {
            return false;
        }
        // Replay only copies that could not be saved, oldest first. Reusing
        // the bounded history for this journal preserves deduplication and
        // prevents stale base entries from undoing another client's deletion.
        for (auto entry = pendingObservations_.entries().rbegin();
             entry != pendingObservations_.entries().rend(); ++entry) {
            latest.observe(*entry);
        }
        history_ = std::move(latest);
    }
    if (!history_.save(path_, error)) {
        return false;
    }
    dirty_ = false;
    pendingObservations_.clear();
    knownFileState_ = currentFileState();
    return true;
}

} // namespace modernime::core
