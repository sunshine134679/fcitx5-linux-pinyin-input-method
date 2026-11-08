#include "modernime/core/async_clipboard_history.h"
#include "modernime/core/clipboard_observation.h"
#include "modernime/core/persistent_clipboard_history.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace modernime::core {

struct AsyncClipboardHistory::Impl {
    struct Task {
        ClipboardObservationBatch observations;
        bool save = false;
        bool final = false;
    };
    struct Mailbox {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<Task> task;
        std::optional<ClipboardSnapshot> result;
        bool closing = false;
        ClipboardObservationBatch finalObservations;
        std::string finalError;
    } mailbox;

    ClipboardObservationTracker tracker;
    ClipboardObservationBatch waiting;
    bool active = true;
    bool refreshRequested = false;
    bool saveRequested = false;
    bool closed = false;
    std::thread worker;

    Impl(std::filesystem::path path, std::function<void()> notify) {
        mailbox.task.emplace(); // Initial loading belongs to the worker too.
        worker = std::thread([this, path = std::move(path), notify = std::move(notify)] {
            run(path, notify);
        });
    }

    void dispatch() {
        if (closed || active || (!refreshRequested && !saveRequested && waiting.empty())) return;
        Task task{std::move(waiting), saveRequested, false};
        waiting = {};
        refreshRequested = false;
        saveRequested = false;
        active = true;
        {
            std::lock_guard lock(mailbox.mutex);
            mailbox.task.emplace(std::move(task));
        }
        mailbox.ready.notify_one();
    }

    void run(const std::filesystem::path &path, const std::function<void()> &notify) {
        PersistentClipboardHistory history(path);
        ClipboardObservationBatch deferred;
        ClipboardSnapshot snapshot;
        snapshot.entries = std::make_shared<const std::vector<std::string>>();
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mailbox.mutex);
                mailbox.ready.wait(lock, [&] { return mailbox.task || mailbox.closing; });
                if (mailbox.task) {
                    task = std::move(*mailbox.task);
                    mailbox.task.reset();
                } else {
                    task = {std::move(mailbox.finalObservations), true, true};
                }
            }
            std::string error;
            auto availability = ClipboardAvailability::Unavailable;
            try {
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
            ++snapshot.version;
            bool shouldNotify;
            {
                std::lock_guard lock(mailbox.mutex);
                mailbox.result = snapshot;
                shouldNotify = !mailbox.closing;
                if (task.final) mailbox.finalError = error;
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
            }
            mailbox.ready.notify_one();
            worker.join();
        }
        return mailbox.finalError;
    }
};

AsyncClipboardHistory::AsyncClipboardHistory(std::filesystem::path path,
                                             std::function<void()> notify)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(notify))) {}
AsyncClipboardHistory::~AsyncClipboardHistory() { impl_->shutdown(); }
void AsyncClipboardHistory::setObservationBaseline(std::string_view text) {
    if (!impl_->closed) impl_->tracker.setBaseline(text);
}
void AsyncClipboardHistory::observe(std::string_view text) {
    if (!impl_->closed && impl_->tracker.accept(text)) impl_->waiting.observe(text);
}
void AsyncClipboardHistory::requestRefresh() {
    if (impl_->closed) return;
    impl_->refreshRequested = true;
    impl_->dispatch();
}
void AsyncClipboardHistory::requestSave() {
    if (impl_->closed) return;
    impl_->saveRequested = true;
    impl_->dispatch();
}
std::optional<ClipboardSnapshot> AsyncClipboardHistory::takeSnapshot() {
    if (impl_->closed) return std::nullopt;
    std::optional<ClipboardSnapshot> result;
    {
        std::lock_guard lock(impl_->mailbox.mutex);
        result = std::move(impl_->mailbox.result);
        impl_->mailbox.result.reset();
    }
    if (result) {
        impl_->active = false;
        impl_->dispatch();
    }
    return result;
}
std::string AsyncClipboardHistory::shutdown() { return impl_->shutdown(); }

} // namespace modernime::core
