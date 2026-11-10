#include "modernime/core/persistent_clipboard_history.h"
#include "../support/clipboard_io_gate.h"
#ifndef MODERNIME_CLIPBOARD_SYNC_BASELINE
#include "modernime/core/async_clipboard_history.h"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace modernime::core;
using namespace std::chrono_literals;
namespace {
void require(bool ok, const char *message) {
    if (!ok) { std::cerr << "async clipboard failed: " << message << '\n'; std::exit(1); }
}
struct Notifications {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned count = 0;
    void notify() { std::lock_guard lock(mutex); ++count; changed.notify_all(); }
};
#ifdef MODERNIME_CLIPBOARD_SYNC_BASELINE
class AsyncClipboardHistory {
public:
    AsyncClipboardHistory(std::filesystem::path path, std::function<void()> notify) : history_(path) {
        history_.load(); notify();
    }
private:
    PersistentClipboardHistory history_;
};
#else
ClipboardSnapshot next(AsyncClipboardHistory &history, Notifications &notifications) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        std::unique_lock lock(notifications.mutex);
        const auto previous = notifications.count;
        lock.unlock();
        if (auto result = history.takeSnapshot()) return *result;
        lock.lock();
        require(notifications.changed.wait_until(lock, deadline, [&] { return notifications.count != previous; }),
                "background operation completes before deadline");
    }
}
std::vector<std::string> disk(const std::filesystem::path &path) {
    ClipboardHistory h; require(h.load(path), "persisted history readable"); return h.entries();
}
#endif
}
int main() {
    char tmp[] = "/tmp/modernime-async-clipboard-XXXXXX";
    require(mkdtemp(tmp) != nullptr, "private root created");
    const auto path = std::filesystem::path(tmp) / "history.bin";
    ClipboardHistory seed; seed.observe("seed"); require(seed.save(path), "seed saved");
    Notifications notifications;
    std::mutex startupMutex;
    std::condition_variable startupChanged;
    bool constructed = false, responsive = false, hit = false;
    modernime_clipboard_gate_arm(path.c_str(), clipboardAfterRead);
    std::thread watchdog([&] {
        hit = modernime_clipboard_gate_wait(3000);
        std::unique_lock lock(startupMutex);
        responsive = startupChanged.wait_for(lock, 300ms, [&] { return constructed; });
        lock.unlock();
        modernime_clipboard_gate_release();
    });
    AsyncClipboardHistory history(path, [&] { notifications.notify(); });
    {
        std::lock_guard lock(startupMutex);
        constructed = true; startupChanged.notify_all();
    }
    watchdog.join();
    require(hit, "actual file read held at completion");
    require(responsive, "constructor returns while history read is held");
#ifndef MODERNIME_CLIPBOARD_SYNC_BASELINE
    auto snapshot = next(history, notifications);
    require(snapshot.availability == ClipboardAvailability::Ready && *snapshot.entries == std::vector<std::string>{"seed"},
            "initial immutable snapshot is ready");
    const auto originalSnapshot = snapshot.entries;
    history.setObservationBaseline("excluded");
    history.observe("excluded"); history.requestRefresh();
    snapshot = next(history, notifications);
    require(disk(path) == std::vector<std::string>{"seed"}, "baseline never persisted");

    modernime_clipboard_gate_arm(path.c_str(), clipboardBeforeFsync);
    history.observe("A"); history.requestRefresh();
    require(modernime_clipboard_gate_wait(3000), "real writer reaches held fsync");
    history.setObservationBaseline("C");
    history.observe("B"); history.observe("A"); history.requestRefresh();
    require(!history.takeSnapshot(), "polling while write held never waits");
    modernime_clipboard_gate_release();
    next(history, notifications); // A committed; this dispatches the waiting batch.
    snapshot = next(history, notifications);
    require(*snapshot.entries == std::vector<std::string>({"A", "B", "seed"}), "new batch survives previous write completion");
    require(*originalSnapshot == std::vector<std::string>{"seed"}, "published old snapshot stays immutable");

    // Re-copy of the previous top after a real external clear must be accepted.
    history.setObservationBaseline("B"); seed.clear(); require(seed.save(path), "external clear succeeds");
    history.observe("A"); history.requestRefresh();
    snapshot = next(history, notifications);
    require(disk(path) == std::vector<std::string>{"A"}, "fresh copy classified after external reload");
    require(std::filesystem::remove(path), "external unlink succeeds");
    history.requestRefresh(); snapshot = next(history, notifications);
    require(snapshot.entries->empty(), "external unlink publishes empty history");

    const auto blocked = path.string() + ".tmp";
    std::filesystem::create_directory(blocked);
    history.observe("pending"); history.requestRefresh();
    snapshot = next(history, notifications);
    require(!snapshot.error.empty(), "failed save is reported");
    history.setObservationBaseline("excluded again");
    history.observe("excluded again");
    std::filesystem::remove(blocked);
    seed.clear(); require(seed.save(path), "clear after failed save succeeds");
    history.requestSave(); snapshot = next(history, notifications);
    require(snapshot.error.empty() && disk(path) == std::vector<std::string>{"pending"}, "retry retains only legitimate pending");

    modernime_clipboard_fail_reads(path.c_str(), true);
    seed.observe("external"); // Save via temporary file is permitted by read fault.
    require(seed.save(path), "replacement for failed reload saved");
    history.requestRefresh(); snapshot = next(history, notifications);
    require(snapshot.availability == ClipboardAvailability::Unavailable && !snapshot.error.empty(), "reload failure is not Ready");
    history.observe("first"); history.observe("second"); history.requestRefresh();
    snapshot = next(history, notifications);
    require(snapshot.availability == ClipboardAvailability::Unavailable, "failed load retains deferred batch");
    history.setObservationBaseline("skip"); history.observe("first");
    modernime_clipboard_fail_reads(path.c_str(), false);
    history.requestRefresh(); snapshot = next(history, notifications);
    require(snapshot.error.empty() && disk(path) == std::vector<std::string>({"first", "second", "external"}),
            "deferred batches compose across initial read failure");

    // A full bounded batch is drained at shutdown, including every original byte.
    std::vector<std::string> full;
    for (int i = 0; i < 35; ++i) {
        std::string value(ClipboardHistory::kMaxEntryBytes, static_cast<char>('a' + i % 26));
        value.replace(0, 4, std::to_string(1000 + i));
        history.observe(value);
        full.insert(full.begin(), value);
        if (full.size() > 30) full.pop_back();
    }
    require(history.shutdown().empty(), "shutdown drains accepted batch");
    require(disk(path) == full, "thirty 1MiB records remain exact after shutdown");
    const auto closedCount = notifications.count;
    history.requestRefresh(); history.observe("after close");
    require(history.shutdown().empty() && notifications.count == closedCount, "closed owner does not notify or write again");

    {
        const auto initialPath = std::filesystem::path(tmp) / "initial.bin";
        ClipboardHistory initial; initial.observe("A"); require(initial.save(initialPath), "initial seed saves");
        Notifications initialNotifications;
        modernime_clipboard_gate_arm(initialPath.c_str(), clipboardAfterRead);
        AsyncClipboardHistory loading(initialPath, [&] { initialNotifications.notify(); });
        require(modernime_clipboard_gate_wait(3000), "initial load held before publication");
        loading.setObservationBaseline("B");
        initial.clear(); require(initial.save(initialPath), "external clear during initial load");
        loading.observe("A"); loading.requestRefresh();
        modernime_clipboard_gate_release();
        next(loading, initialNotifications);
        next(loading, initialNotifications);
        require(disk(initialPath) == std::vector<std::string>{"A"}, "copy during initial load survives old-top and clear race");
    }

    {
        const auto unavailablePath = std::filesystem::path(tmp) / "unavailable.bin";
        ClipboardHistory initial; initial.observe("existing"); require(initial.save(unavailablePath), "unavailable seed saves");
        Notifications initialNotifications;
        modernime_clipboard_fail_reads(unavailablePath.c_str(), true);
        AsyncClipboardHistory unavailable(unavailablePath, [&] { initialNotifications.notify(); });
        require(next(unavailable, initialNotifications).availability == ClipboardAvailability::Unavailable,
                "initial failure is explicit");
        unavailable.setObservationBaseline("excluded");
        unavailable.observe("A"); unavailable.observe("B"); unavailable.requestRefresh();
        require(next(unavailable, initialNotifications).availability == ClipboardAvailability::Unavailable,
                "deferred initial observations survive another failed read");
        unavailable.observe("A"); unavailable.requestRefresh();
        modernime_clipboard_fail_reads(unavailablePath.c_str(), false);
        auto recovered = next(unavailable, initialNotifications);
        if (recovered.availability != ClipboardAvailability::Ready) {
            unavailable.requestRefresh(); recovered = next(unavailable, initialNotifications);
        }
        require(recovered.availability == ClipboardAvailability::Ready &&
                    disk(unavailablePath) == std::vector<std::string>({"A", "B", "existing"}),
                "initial read recovery composes all accepted batches");
    }

    {
        const auto shutdownPath = std::filesystem::path(tmp) / "shutdown.bin";
        modernime_clipboard_gate_arm(shutdownPath.c_str(), clipboardBeforeFsync);
        // One client thread owns every public operation; the test thread only
        // controls the actual I/O gate and observes completion.
        auto closing = std::async(std::launch::async, [&] {
            Notifications clientNotifications;
            AsyncClipboardHistory client(shutdownPath, [&] { clientNotifications.notify(); });
            next(client, clientNotifications);
            client.setObservationBaseline("baseline"); client.observe("in flight"); client.requestRefresh();
            return client.shutdown();
        });
        const bool held = modernime_clipboard_gate_wait(3000);
        const bool waiting = closing.wait_for(50ms) == std::future_status::timeout;
        modernime_clipboard_gate_release();
        require(held && waiting && closing.get().empty(), "shutdown joins the real in-flight writer");
        require(disk(shutdownPath) == std::vector<std::string>{"in flight"}, "shutdown keeps accepted in-flight copy");
    }

    const auto errorPath = std::filesystem::path(tmp) / "error.bin";
    Notifications failureNotifications;
    AsyncClipboardHistory failing(errorPath, [&] { failureNotifications.notify(); });
    next(failing, failureNotifications);
    std::filesystem::create_directory(errorPath.string() + ".tmp");
    failing.setObservationBaseline("base"); failing.observe("legitimate");
    require(!failing.shutdown().empty(), "permanent save failure reported by finite shutdown");
#endif
    std::filesystem::remove_all(tmp);
    std::cout << "async clipboard lifecycle passed\n";
}
