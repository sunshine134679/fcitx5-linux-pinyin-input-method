#include "modernime/settings/settings_widgets.h"

#include <cassert>
#include <string_view>

int main() {
    using namespace modernime::settings;
    static_assert(kSettingsWindowClass == "modernime-settings");
    static_assert(kSettingsSidebarClass == "modernime-sidebar");
    static_assert(kSettingsSectionClass == "modernime-section");
    static_assert(kSettingsPrimaryButtonClass == "suggested-action");
    static_assert(kSettingsDangerButtonClass == "destructive-action");
    static_assert(kSettingsFocusFallbackClass ==
                  "modernime-focus-fallback");
    assert(settingsStyles().find("@theme_bg_color") != std::string_view::npos);
    assert(settingsStyles().find(".modernime-focus-fallback") !=
           std::string_view::npos);
    assert(settingsStyles().find("linear-gradient") == std::string_view::npos);
}
