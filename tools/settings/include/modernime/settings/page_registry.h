#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace modernime::settings {

enum class SettingsPageId {
    Overview,
    Input,
    Dictionary,
    Clipboard,
    Learning,
    Diagnostics,
};

struct SettingsPageDefinition final {
    SettingsPageId id;
    std::string_view name;
    std::string_view title;
    std::string_view subtitle;
    std::string_view group;
};

struct SettingsSearchEntry final {
    SettingsPageId page;
    std::string_view target;
    std::string_view title;
    std::string_view description;
    std::string_view keywords;
};

std::span<const SettingsPageDefinition> settingsPageDefinitions();
std::span<const SettingsSearchEntry> settingsSearchEntries();
std::vector<SettingsSearchEntry> searchSettings(std::string_view query);

} // namespace modernime::settings
