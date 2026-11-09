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

bool PersistentClipboardHistory::load(std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    const auto state = currentFileState();
    loaded_ = history_.load(path_, error, /*recoverCorrupt=*/true);
    dirty_ = false;
    pendingObservations_.clear();
    knownFileState_ = loaded_ ? std::optional(state) : std::nullopt;
    return loaded_;
}

void PersistentClipboardHistory::setObservationBaseline(std::string_view text) {
    observationTracker_.setBaseline(text);
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

    ClipboardObservationBatch batch;
    if (observationTracker_.accept(text)) batch.observe(text);
    return observeBatch(batch, error);
}

bool PersistentClipboardHistory::observeBatch(const ClipboardObservationBatch &batch,
                                               std::string *error) {
    if (error != nullptr) error->clear();
    if (!loaded_) {
        setError(error, "clipboard history is not loaded");
        return false;
    }
    const bool changed = batch.apply(history_, pendingObservations_);
    dirty_ = dirty_ || changed;
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
    if (loaded_ && knownFileState_ && state == *knownFileState_) {
        return false;
    }
    loaded_ = history_.load(path_, error, /*recoverCorrupt=*/true);
    dirty_ = false;
    knownFileState_ = loaded_ ? std::optional(state) : std::nullopt;
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
    if (!dirty_ && pendingObservations_.entries().empty()) {
        return true;
    }
    if (persist(error)) {
        return true;
    }
    // If non-blocking persist failed due to lock contention, perform a blocking update
    ClipboardHistory committed;
    const bool ok = ClipboardHistory::updateFile(
        path_,
        [this](ClipboardHistory &diskHistory) {
            for (auto entry = pendingObservations_.entries().rbegin();
                 entry != pendingObservations_.entries().rend(); ++entry) {
                diskHistory.observe(*entry);
            }
            return true;
        },
        &committed,
        error,
        /*nonBlocking=*/false);
    if (!ok) {
        return false;
    }
    history_ = std::move(committed);
    dirty_ = false;
    pendingObservations_.clear();
    knownFileState_.reset();
    return true;
}

bool PersistentClipboardHistory::persist(std::string *error) {
    if (!dirty_ && pendingObservations_.entries().empty()) {
        return true;
    }

    ClipboardHistory committed;
    const bool ok = ClipboardHistory::updateFile(
        path_,
        [this](ClipboardHistory &diskHistory) {
            for (auto entry = pendingObservations_.entries().rbegin();
                 entry != pendingObservations_.entries().rend(); ++entry) {
                diskHistory.observe(*entry);
            }
            return true;
        },
        &committed,
        error,
        /*nonBlocking=*/true);

    if (!ok) {
        return false;
    }

    history_ = std::move(committed);
    dirty_ = false;
    pendingObservations_.clear();
    // An external writer may already have replaced the just-committed file.
    // Recheck on the next refresh; never stamp an old result with a later file.
    knownFileState_.reset();
    return true;
}

} // namespace modernime::core
