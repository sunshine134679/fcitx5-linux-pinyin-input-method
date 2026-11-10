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
};

// All public methods belong to one host thread. File operations belong solely
// to the worker. notify runs on that worker and must marshal to the host loop.
class AsyncClipboardHistory final {
public:
    AsyncClipboardHistory(std::filesystem::path path, std::function<void()> notify);
    ~AsyncClipboardHistory();
    AsyncClipboardHistory(const AsyncClipboardHistory &) = delete;
    AsyncClipboardHistory &operator=(const AsyncClipboardHistory &) = delete;

    void setObservationBaseline(std::string_view text);
    // Collects an observation; requestRefresh/Save schedules an idle batch.
    // An in-flight completion and shutdown also drain accepted observations.
    void observe(std::string_view text);
    void requestRefresh();
    void requestSave();
    std::optional<ClipboardSnapshot> takeSnapshot();
    // Only shutdown may wait for I/O. Drains accepted copies, returns the last
    // persistence error, and joins before the owner/notification target dies.
    std::string shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::core
