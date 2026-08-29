#pragma once

#include "modernime/core/settings.h"
#include "modernime/settings/page_registry.h"
#include "modernime/settings/runtime_controller.h"

#include <cstddef>
#include <string>
#include <vector>

namespace modernime::settings {

struct OverviewNotice final {
    SettingsPageId destination;
    std::string message;
};

struct OverviewSnapshot final {
    std::string runtimeSummary;
    std::string defaultMode;
    std::string toggleKey;
    std::size_t dictionaryEntries = 0;
    std::size_t clipboardEntries = 0;
    std::size_t learningEntries = 0;
    std::vector<OverviewNotice> notices;
};

OverviewSnapshot collectOverviewSnapshot(
    const core::SettingsPaths &paths,
    const core::ModernIMESettings &settings,
    const RuntimeStatus &runtime);

} // namespace modernime::settings
