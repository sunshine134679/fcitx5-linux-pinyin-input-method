#pragma once

#include "modernime/core/input_compatibility.h"
#include "modernime/core/user_data_bundle.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace modernime::core { class LearningWriter; }
namespace modernime::fcitx5 {

// One private EventLoop/DBus connection owns all management work. Callbacks must
// remain valid until destruction; getWriter is thread safe. Maintenance begin
// acknowledges the main-loop reload fence; end schedules its release/refresh.
class BackendService final {
public:
    struct Config final {
        core::UserDataPaths paths;
        std::filesystem::path packRoot;
        std::filesystem::path bundledRoot;
        std::function<std::shared_ptr<core::LearningWriter>()> getWriter;
        std::shared_ptr<core::InputCompatibilityTracker> compatibility;
        std::function<bool()> beginMaintenance;
        std::function<void()> endMaintenance;
    };
    explicit BackendService(Config config);
    ~BackendService();
    BackendService(const BackendService &) = delete;
    BackendService &operator=(const BackendService &) = delete;
    bool ready() const noexcept;
    std::string startupError() const;
    static constexpr const char *serviceName = "org.modernime.Backend1";
    static constexpr const char *objectPath = "/org/modernime/Backend1";
    static constexpr const char *interfaceName = "org.modernime.Backend1";
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace modernime::fcitx5
