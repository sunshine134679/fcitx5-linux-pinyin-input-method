#pragma once

#include "modernime/settings/page_registry.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace modernime::settings {

enum class SettingsFocusDestination {
    None,
    Target,
    Fallback,
};

SettingsFocusDestination chooseSettingsFocusDestination(
    bool targetFocusable, bool fallbackFocusable);

struct SettingsFocusRoute final {
    SettingsPageId page;
    std::string_view target;
};

std::optional<SettingsFocusRoute>
settingsFocusRouteForIssue(std::string_view issueKey);

class OverviewRefreshState final {
public:
    using Generation = std::uint64_t;

    std::optional<Generation> request();
    std::optional<Generation> startPending();
    bool complete(Generation generation);
    Generation generation() const { return generation_; }

private:
    Generation generation_ = 0;
    bool busy_ = false;
    bool pending_ = false;
};

template <typename Lifetime>
class ScopedLifetimeDeactivation final {
public:
    explicit ScopedLifetimeDeactivation(std::shared_ptr<Lifetime> state)
        : state_(std::move(state)) {}

    ~ScopedLifetimeDeactivation() {
        if (state_ != nullptr) {
            state_->deactivate();
        }
    }

    ScopedLifetimeDeactivation(const ScopedLifetimeDeactivation &) = delete;
    ScopedLifetimeDeactivation &
    operator=(const ScopedLifetimeDeactivation &) = delete;

    const std::shared_ptr<Lifetime> &state() const { return state_; }

private:
    std::shared_ptr<Lifetime> state_;
};

} // namespace modernime::settings
