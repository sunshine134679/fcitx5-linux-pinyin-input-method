#include "modernime/core/async_clipboard_history.h"
#include "modernime/core/clipboard_observation.h"
#include "modernime/core/clipboard_queries.h"
#include "modernime/core/persistent_clipboard_history.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <utility>

namespace modernime::core {
namespace {

struct FavoritesFileStamp {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t size = 0;
    std::chrono::nanoseconds modified{};
    std::chrono::nanoseconds changed{};
    bool exists = false;

    bool operator==(const FavoritesFileStamp &) const = default;
};

std::optional<FavoritesFileStamp> favoritesFileStamp(const std::filesystem::path &path) {
    struct stat metadata {};
    if (::stat(path.c_str(), &metadata) == 0) {
        return FavoritesFileStamp{
            static_cast<std::uintmax_t>(metadata.st_dev),
            static_cast<std::uintmax_t>(metadata.st_ino),
            static_cast<std::uintmax_t>(metadata.st_size),
            std::chrono::seconds(metadata.st_mtim.tv_sec) +
                std::chrono::nanoseconds(metadata.st_mtim.tv_nsec),
            std::chrono::seconds(metadata.st_ctim.tv_sec) +
                std::chrono::nanoseconds(metadata.st_ctim.tv_nsec),
            true};
    }
    // Missing is a known empty file state. Other stat failures must be retried.
    if (errno == ENOENT) return FavoritesFileStamp{};
    return std::nullopt;
}

} // namespace

struct AsyncClipboardHistory::Impl {
    struct FavoriteOperation {
        std::uint64_t id = 0;
        std::string text;
        bool favorite = false;
    };
    struct Task {
        ClipboardObservationBatch observations;
        bool save = false;
        bool final = false;
        std::vector<FavoriteOperation> favoriteOperations;
        bool pause = false;
        bool reloadFromDisk = false;
    };
    struct Mailbox {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<Task> task;
        std::optional<ClipboardSnapshot> result;
        bool completionPending = false;
        bool maintenancePaused = false;
        bool closing = false;
        ClipboardObservationBatch finalObservations;
        std::vector<FavoriteOperation> finalFavoriteOperations;
        std::string finalError;
    } mailbox;

    ClipboardObservationTracker tracker;
    ClipboardObservationBatch waiting;
    std::vector<FavoriteOperation> waitingFavoriteOperations;
    std::uint64_t acceptedFavoriteOperationId = 0;
    std::uint64_t completedFavoriteOperationId = 0;
    bool favoritesEnabled = false;
    bool active = true;
    bool refreshRequested = false;
    bool saveRequested = false;
    bool closed = false;
    bool pauseRequested = false;
    bool maintenancePaused = false;
    bool reloadFromDiskRequested = false;
    std::thread worker;

    Impl(std::filesystem::path path, std::function<void()> notify,
         std::filesystem::path favoritesPath)
        : favoritesEnabled(!favoritesPath.empty()) {
        mailbox.task.emplace(); // Initial loading belongs to the worker too.
        worker = std::thread([this, path = std::move(path), notify = std::move(notify),
                              favoritesPath = std::move(favoritesPath)] {
            run(path, favoritesPath, notify);
        });
    }

    void dispatch() {
        if (closed || active || (maintenancePaused && !reloadFromDiskRequested) ||
            (!pauseRequested && !refreshRequested && !saveRequested && waiting.empty() &&
                                 waitingFavoriteOperations.empty())) return;
        Task task{std::move(waiting), saveRequested || pauseRequested, false,
                  std::move(waitingFavoriteOperations), pauseRequested, reloadFromDiskRequested};
        waiting = {};
        waitingFavoriteOperations = {};
        refreshRequested = false;
        saveRequested = false;
        reloadFromDiskRequested = false;
        active = true;
        {
            std::lock_guard lock(mailbox.mutex);
            mailbox.task.emplace(std::move(task));
        }
        mailbox.ready.notify_one();
    }

    // A maintenance poll acknowledges completion without consuming the result
    // intended for the host UI. The single latest snapshot stays in mailbox.
    std::optional<ClipboardSnapshot> collect(bool consume) {
        std::optional<ClipboardSnapshot> result;
        {
            std::lock_guard lock(mailbox.mutex);
            if (mailbox.completionPending) {
                if (mailbox.result) completedFavoriteOperationId = mailbox.result->lastFavoriteOperationId;
                active = false;
                maintenancePaused = mailbox.maintenancePaused;
                mailbox.completionPending = false;
            }
            if (consume) {
                result = std::move(mailbox.result);
                mailbox.result.reset();
            }
        }
        dispatch();
        return result;
    }

    void run(const std::filesystem::path &path, const std::filesystem::path &favoritesPath,
             const std::function<void()> &notify) {
        PersistentClipboardHistory history(path);
        ClipboardFavorites favorites(favoritesPath);
        std::optional<FavoritesFileStamp> knownFavoritesStamp;
        ClipboardObservationBatch deferred;
        ClipboardSnapshot snapshot;
        snapshot.entries = std::make_shared<const std::vector<std::string>>();
        snapshot.favorites = std::make_shared<const std::vector<std::string>>();
        if (favoritesPath.empty()) snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
        for (;;) {
            Task task;
            bool resetCleanCaches = false;
            {
                std::unique_lock lock(mailbox.mutex);
                mailbox.ready.wait(lock, [&] { return mailbox.task || mailbox.closing; });
                if (mailbox.task) {
                    task = std::move(*mailbox.task);
                    mailbox.task.reset();
                    resetCleanCaches = task.reloadFromDisk && mailbox.maintenancePaused;
                } else {
                    // A successful maintenance pause has no pending facts.
                    // In particular, shutdown must not flush pre-restore cache.
                    if (mailbox.maintenancePaused) return;
                    task = {std::move(mailbox.finalObservations), true, true,
                            std::move(mailbox.finalFavoriteOperations)};
                }
            }
            std::string error;
            auto availability = ClipboardAvailability::Unavailable;
            try {
                if (resetCleanCaches) {
                    history = PersistentClipboardHistory(path);
                    favorites = ClipboardFavorites(favoritesPath);
                    knownFavoritesStamp.reset();
                }
                deferred.append(task.observations);
                if (!history.loaded()) history.load(&error);
                else history.reloadIfExternallyChanged(&error);
                if (history.loaded()) {
                    history.observeBatch(deferred, &error);
                    deferred = {};
                    if (task.save) history.flush(&error);
                    availability = ClipboardAvailability::Ready;
                    if (*snapshot.entries != history.entries())
                        snapshot.entries = std::make_shared<const std::vector<std::string>>(history.entries());
                }
            } catch (const std::exception &exception) {
                error = exception.what();
            } catch (...) {
                error = "unexpected clipboard history failure";
            }
            snapshot.availability = availability;
            snapshot.error = error;
            if (task.pause && !error.empty()) snapshot.availability = ClipboardAvailability::Unavailable;
            if (!favoritesPath.empty()) {
                bool favoritesChanged = false;
                std::string readError;
                std::string batchError;
                try {
                    // Capture before the read: an external replacement during
                    // loading must still be detected on the following refresh.
                    const auto stamp = favoritesFileStamp(favoritesPath);
                    if (!knownFavoritesStamp || !stamp || *knownFavoritesStamp != *stamp) {
                        if (favorites.load(&readError)) {
                            knownFavoritesStamp = stamp;
                            favoritesChanged = true;
                            snapshot.favoritesAvailability = ClipboardAvailability::Ready;
                        } else {
                            knownFavoritesStamp.reset();
                            snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
                        }
                    }
                } catch (const std::exception &exception) {
                    readError = exception.what();
                    knownFavoritesStamp.reset();
                    snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
                } catch (...) {
                    readError = "unexpected clipboard favorites read failure";
                    knownFavoritesStamp.reset();
                    snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
                }
                for (const auto &operation : task.favoriteOperations) {
                    std::string operationError;
                    bool success = false;
                    try {
                        success = operation.favorite ? favorites.add(operation.text, &operationError)
                                                     : favorites.remove(operation.text, &operationError);
                    } catch (const std::exception &exception) {
                        operationError = exception.what();
                    } catch (...) {
                        operationError = "unexpected clipboard favorites write failure";
                    }
                    snapshot.lastFavoriteOperationId = operation.id;
                    snapshot.lastFavoriteOperationSucceeded = success;
                    if (success) {
                        favoritesChanged = true;
                        readError.clear();
                        snapshot.favoritesAvailability = ClipboardAvailability::Ready;
                    } else {
                        if (operationError.empty()) operationError = "unable to update clipboard favorites";
                        if (batchError.empty()) batchError = operationError;
                    }
                    snapshot.lastFavoriteOperationError = std::move(operationError);
                    // updateFile reads its own locked state. Do not label that
                    // result with a possibly newer external file's stamp.
                    knownFavoritesStamp.reset();
                }
                snapshot.favoritesError = !batchError.empty() ? batchError
                    : !readError.empty() ? readError
                    : task.reloadFromDisk ? std::string{} : snapshot.lastFavoriteOperationError;
                if (favoritesChanged) {
                    try {
                        if (*snapshot.favorites != favorites.entries()) {
                            snapshot.favorites = std::make_shared<const std::vector<std::string>>(favorites.entries());
                        }
                    } catch (const std::exception &exception) {
                        snapshot.favoritesError = exception.what();
                        snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
                        knownFavoritesStamp.reset();
                    } catch (...) {
                        snapshot.favoritesError = "unable to publish clipboard favorites snapshot";
                        snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
                        knownFavoritesStamp.reset();
                    }
                }
                if (task.pause && !snapshot.favoritesError.empty())
                    snapshot.favoritesAvailability = ClipboardAvailability::Unavailable;
            }
            ++snapshot.version;
            bool shouldNotify;
            {
                std::lock_guard lock(mailbox.mutex);
                mailbox.result = snapshot;
                mailbox.completionPending = true;
                if (task.reloadFromDisk) mailbox.maintenancePaused = false;
                if (task.pause) {
                    mailbox.maintenancePaused = snapshot.availability == ClipboardAvailability::Ready &&
                        error.empty() && deferred.empty() &&
                        (favoritesPath.empty() || (snapshot.favoritesAvailability == ClipboardAvailability::Ready &&
                                                  snapshot.favoritesError.empty()));
                }
                shouldNotify = !mailbox.closing;
                if (task.final) {
                    mailbox.finalError = error;
                    if (!snapshot.favoritesError.empty()) {
                        if (!mailbox.finalError.empty()) mailbox.finalError += "; ";
                        mailbox.finalError += "clipboard favorites: " + snapshot.favoritesError;
                    }
                }
            }
            if (task.final) return;
            if (shouldNotify && notify) {
                try {
                    notify();
                } catch (...) {
                    // A host notification failure must not terminate persistence.
                    // Its timer can still consume the completed result.
                    std::lock_guard lock(mailbox.mutex);
                    if (mailbox.result) mailbox.result->error = "unable to notify clipboard history completion";
                }
            }
        }
    }

    std::string shutdown() {
        if (!closed) {
            closed = true;
            {
                std::lock_guard lock(mailbox.mutex);
                mailbox.closing = true;
                mailbox.finalObservations = std::move(waiting);
                mailbox.finalFavoriteOperations = std::move(waitingFavoriteOperations);
            }
            mailbox.ready.notify_one();
            worker.join();
        }
        return mailbox.finalError;
    }
};

AsyncClipboardHistory::AsyncClipboardHistory(std::filesystem::path path,
                                             std::function<void()> notify,
                                             std::filesystem::path favoritesPath)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(notify), std::move(favoritesPath))) {}
AsyncClipboardHistory::~AsyncClipboardHistory() { impl_->shutdown(); }
void AsyncClipboardHistory::setObservationBaseline(std::string_view text) {
    if (!impl_->closed) impl_->tracker.setBaseline(text);
}
void AsyncClipboardHistory::observe(std::string_view text) {
    if (!impl_->closed && !impl_->pauseRequested && impl_->tracker.accept(text)) impl_->waiting.observe(text);
}
void AsyncClipboardHistory::requestRefresh() {
    if (impl_->closed || impl_->pauseRequested) return;
    impl_->refreshRequested = true;
    impl_->dispatch();
}
void AsyncClipboardHistory::requestSave() {
    if (impl_->closed || impl_->pauseRequested) return;
    impl_->saveRequested = true;
    impl_->dispatch();
}
bool AsyncClipboardHistory::setFavorite(std::string_view text, bool favorite) {
    if (impl_->closed || impl_->pauseRequested || !impl_->favoritesEnabled || text.empty() ||
        text.size() > ClipboardHistory::kMaxEntryBytes ||
        impl_->acceptedFavoriteOperationId - impl_->completedFavoriteOperationId >= ClipboardHistory::kMaxEntries) {
        return false;
    }
    const auto id = impl_->acceptedFavoriteOperationId + 1;
    impl_->waitingFavoriteOperations.push_back({id, std::string(text), favorite});
    impl_->acceptedFavoriteOperationId = id;
    impl_->dispatch();
    return true;
}
std::uint64_t AsyncClipboardHistory::lastAcceptedFavoriteOperationId() const {
    return impl_->acceptedFavoriteOperationId;
}
std::optional<ClipboardSnapshot> AsyncClipboardHistory::takeSnapshot() {
    if (impl_->closed) return std::nullopt;
    return impl_->collect(true);
}
std::string AsyncClipboardHistory::shutdown() { return impl_->shutdown(); }
bool AsyncClipboardHistory::pauseForMaintenance() {
    if (impl_->closed) return false;
    impl_->pauseRequested = true;
    impl_->collect(false);
    return impl_->maintenancePaused;
}
void AsyncClipboardHistory::resumeAfterMaintenance() {
    if (impl_->closed || !impl_->pauseRequested) return;
    // Failed/cancelled pauses still own legitimate pending facts: leave their
    // persistent object intact. Only a successful drain permits cache reset.
    impl_->collect(false);
    impl_->pauseRequested = false;
    // The preceding pause may still be in flight. The worker decides whether
    // it actually acknowledged a clean pause before resetting any cache.
    impl_->reloadFromDiskRequested = true;
    impl_->refreshRequested = true;
    if (impl_->maintenancePaused) {
        std::lock_guard lock(impl_->mailbox.mutex);
        impl_->mailbox.result.reset();
    }
    impl_->dispatch();
}

} // namespace modernime::core
