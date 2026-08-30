#include "modernime/settings/settings_shell_state.h"

namespace modernime::settings {

SettingsFocusDestination chooseSettingsFocusDestination(
    bool targetFocusable, bool fallbackFocusable) {
    if (targetFocusable) {
        return SettingsFocusDestination::Target;
    }
    if (fallbackFocusable) {
        return SettingsFocusDestination::Fallback;
    }
    return SettingsFocusDestination::None;
}

std::optional<SettingsFocusRoute>
settingsFocusRouteForIssue(std::string_view issueKey) {
    if (issueKey == "input.toggle_key") {
        return SettingsFocusRoute{SettingsPageId::Input, "toggle-key"};
    }
    if (issueKey == "clipboard.trigger") {
        return SettingsFocusRoute{SettingsPageId::Clipboard,
                                  "clipboard-trigger"};
    }
    return std::nullopt;
}

std::optional<OverviewRefreshState::Generation>
OverviewRefreshState::request() {
    ++generation_;
    pending_ = true;
    return startPending();
}

std::optional<OverviewRefreshState::Generation>
OverviewRefreshState::startPending() {
    if (busy_ || !pending_) {
        return std::nullopt;
    }
    busy_ = true;
    pending_ = false;
    return generation_;
}

bool OverviewRefreshState::complete(Generation generation) {
    busy_ = false;
    return generation == generation_;
}

} // namespace modernime::settings
