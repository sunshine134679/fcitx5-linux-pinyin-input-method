#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

enum class ClipboardAvailability { Loading, Ready, Unavailable };
struct ClipboardSnapshot {
    std::shared_ptr<const std::vector<std::string>> entries;
    ClipboardAvailability availability = ClipboardAvailability::Loading;
    std::uint64_t version = 0;
    std::string error;
    std::shared_ptr<const std::vector<std::string>> favorites;
    ClipboardAvailability favoritesAvailability = ClipboardAvailability::Loading;
    std::string favoritesError;
    // Zero means no favorite operation has completed. IDs increase in accepted
    // request order. These fields describe the last completed operation;
    // favoritesError also reports any earlier failure in the same batch.
    std::uint64_t lastFavoriteOperationId = 0;
    bool lastFavoriteOperationSucceeded = true;
    std::string lastFavoriteOperationError;
};

// All public methods belong to one host thread. File operations belong solely
// to the worker. notify runs on that worker and must marshal to the host loop.
class AsyncClipboardHistory final {
public:
    AsyncClipboardHistory(std::filesystem::path path, std::function<void()> notify,
                          std::filesystem::path favoritesPath = {});
    ~AsyncClipboardHistory();
    AsyncClipboardHistory(const AsyncClipboardHistory &) = delete;
    AsyncClipboardHistory &operator=(const AsyncClipboardHistory &) = delete;

    void setObservationBaseline(std::string_view text);
    // Collects an observation; requestRefresh/Save schedules an idle batch.
    // An in-flight completion and shutdown also drain accepted observations.
    void observe(std::string_view text);
    void requestRefresh();
    void requestSave();
    // Queues a full-text favorite add/remove without file I/O on the caller.
    // An empty favorites path disables this API. Returns false after shutdown,
    // for invalid text, or with 30 accepted operations still awaiting snapshot
    // consumption. Accepted requests run once on the existing worker; consume
    // snapshots for persistence results and to dispatch the next waiting batch.
    bool setFavorite(std::string_view text, bool favorite);
    // Host-thread acceptance ID. Rejected requests leave this value unchanged;
    // lastFavoriteOperationId >= this ID confirms its worker batch completed.
    std::uint64_t lastAcceptedFavoriteOperationId() const;
    std::optional<ClipboardSnapshot> takeSnapshot();
    // Host-thread polling only; never waits for I/O. Stops accepting new work,
    // drains accepted observations/operations and confirms a successful flush.
    // True means the worker no longer touches either file until resume.
    bool pauseForMaintenance();
    // Nonblocking. Discards clean old file caches and schedules a forced read
    // of the restored files on the existing worker before publishing Ready.
    void resumeAfterMaintenance();
    // Only shutdown may wait for I/O. Drains accepted copies, returns the last
    // persistence error, and joins before the owner/notification target dies.
    std::string shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::core
