#include "modernime/core/async_clipboard_history.h"
#include "modernime/core/clipboard_queries.h"
#include "../support/clipboard_io_gate.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace modernime::core;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "async clipboard favorites failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Notifications {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;

    void notify() {
        std::lock_guard lock(mutex);
        ++count;
        changed.notify_all();
    }
};

ClipboardSnapshot next(AsyncClipboardHistory &history, Notifications &notifications) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        std::unique_lock lock(notifications.mutex);
        const auto previous = notifications.count;
        lock.unlock();
        if (auto snapshot = history.takeSnapshot()) return *snapshot;
        lock.lock();
        require(notifications.changed.wait_until(lock, deadline, [&] {
                    return notifications.count != previous;
                }), "worker publishes a completed snapshot");
    }
}

bool has(const std::vector<std::string> &entries, std::string_view text) {
    return std::find(entries.begin(), entries.end(), text) != entries.end();
}

} // namespace

int main() {
    char temporary[] = "/tmp/modernime-favorites-async-XXXXXX";
    require(::mkdtemp(temporary) != nullptr, "isolated directory is created");
    const auto directory = std::filesystem::path(temporary);
    const auto path = directory / "history.bin";
    const auto favoritesPath = directory / "favorites.bin";
    ClipboardHistory seed;
    seed.observe("history only");
    require(seed.save(path), "history fixture saves");
    Notifications notifications;
    AsyncClipboardHistory history(path, [&] { notifications.notify(); }, favoritesPath);
    auto snapshot = next(history, notifications);
    require(snapshot.availability == ClipboardAvailability::Ready,
            "legacy history snapshot remains ready");
    require(history.setFavorite("kept", true), "valid favorite request is queued");
    snapshot = next(history, notifications);
    require(snapshot.favorites && *snapshot.favorites == std::vector<std::string>{"kept"} &&
                snapshot.favoritesAvailability == ClipboardAvailability::Ready &&
                snapshot.favoritesError.empty() && snapshot.lastFavoriteOperationId == 1 &&
                snapshot.lastFavoriteOperationSucceeded,
            "favorite completion publishes a separate immutable snapshot");
    const auto originalFavorites = snapshot.favorites;
    ClipboardFavorites disk(favoritesPath);
    require(disk.load() && disk.entries() == *snapshot.favorites,
            "favorite completion is already persisted");

    // Hold a real favorite save, then queue a full bounded batch on the host.
    modernime_clipboard_gate_arm(favoritesPath.c_str(), clipboardBeforeFsync);
    std::mutex responsiveMutex;
    std::condition_variable responsiveChanged;
    bool queued = false;
    bool held = false;
    bool responsive = false;
    std::thread watchdog([&] {
        held = modernime_clipboard_gate_wait(3000);
        std::unique_lock lock(responsiveMutex);
        responsive = responsiveChanged.wait_for(lock, 300ms, [&] { return queued; });
        lock.unlock();
        modernime_clipboard_gate_release();
    });
    require(history.setFavorite("queued-0", true), "first held operation is queued");
    for (unsigned i = 1; i < 29; ++i) {
        require(history.setFavorite("queued-" + std::to_string(i), true),
                "requests queue while the worker is blocked");
    }
    // The last accepted operation fills the 30-operation pending queue while
    // remaining idempotent in the 30-entry favorite store.
    require(history.setFavorite("kept", true), "thirtieth pending request is accepted");
    const auto acceptedId = history.lastAcceptedFavoriteOperationId();
    require(!history.setFavorite("queue overflow", true), "pending queue rejects its thirty-first request");
    require(acceptedId == 31 && history.lastAcceptedFavoriteOperationId() == acceptedId,
            "acceptance ID advances only for accepted requests");
    {
        std::lock_guard lock(responsiveMutex);
        queued = true;
        responsiveChanged.notify_all();
    }
    watchdog.join();
    require(held && responsive, "favorite calls return while real file I/O is held");
    next(history, notifications); // Consuming the in-flight result dispatches waiting requests.
    snapshot = next(history, notifications);
    require(snapshot.lastFavoriteOperationId == 31 && snapshot.lastFavoriteOperationSucceeded &&
                snapshot.favorites->size() == 30 && snapshot.favoritesError.empty(),
            "queued operations drain in accepted order");
    require(*originalFavorites == std::vector<std::string>{"kept"},
            "a published favorite snapshot stays immutable");

    require(history.setFavorite("over capacity", true), "full-store attempt is queued");
    snapshot = next(history, notifications);
    require(!snapshot.lastFavoriteOperationSucceeded && !snapshot.favoritesError.empty() &&
                !snapshot.lastFavoriteOperationError.empty() && snapshot.error.empty() &&
                snapshot.favorites->size() == 30,
            "capacity failure is reported independently of history status");
    seed.clear();
    require(seed.save(path), "external history clear saves");
    history.requestRefresh();
    snapshot = next(history, notifications);
    require(snapshot.entries->empty() && snapshot.favorites->size() == 30,
            "clearing history preserves favorites");

    require(disk.remove("kept"), "external client removes a favorite");
    history.requestRefresh();
    snapshot = next(history, notifications);
    require(snapshot.favoritesAvailability == ClipboardAvailability::Ready &&
                !has(*snapshot.favorites, "kept") && !has(*snapshot.favorites, "over capacity"),
            "external removal refreshes without retrying the failed add");

    require(disk.remove("queued-0"), "another external edit saves");
    modernime_clipboard_fail_reads(favoritesPath.c_str(), true);
    history.requestRefresh();
    snapshot = next(history, notifications);
    require(snapshot.favoritesAvailability == ClipboardAvailability::Unavailable &&
                !snapshot.favoritesError.empty() && has(*snapshot.favorites, "queued-0") &&
                snapshot.error.empty(),
            "failed favorite reads retain the old visible snapshot");
    modernime_clipboard_fail_reads(favoritesPath.c_str(), false);
    history.requestRefresh();
    snapshot = next(history, notifications);
    require(snapshot.favoritesAvailability == ClipboardAvailability::Ready &&
                !has(*snapshot.favorites, "queued-0"),
            "next refresh retries a failed read without another file change");

    require(!history.setFavorite("", true), "invalid favorite text is rejected before queueing");
    require(history.setFavorite("final-one", true) && history.setFavorite("final-two", true),
            "final in-flight and waiting operations are accepted");
    require(history.shutdown().empty(), "shutdown drains accepted favorite operations");
    require(disk.load() && has(disk.entries(), "final-one") && has(disk.entries(), "final-two") &&
                disk.entries().size() == 30,
            "shutdown persists both the dispatched and waiting queues");
    require(!history.setFavorite("after close", true), "closed owners reject new operations");

    {
        Notifications legacyNotifications;
        AsyncClipboardHistory legacy(directory / "legacy.bin", [&] { legacyNotifications.notify(); });
        next(legacy, legacyNotifications);
        require(!legacy.setFavorite("disabled", true), "legacy constructor leaves favorites disabled");
    }
    std::filesystem::remove_all(directory);
    std::cout << "async clipboard favorites passed\n";
    return EXIT_SUCCESS;
}
