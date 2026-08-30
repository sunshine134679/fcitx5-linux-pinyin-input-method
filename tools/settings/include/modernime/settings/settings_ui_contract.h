#pragma once

#include <array>
#include <string_view>

namespace modernime::settings {

struct SettingsWindowSize final {
    int width;
    int height;
};

constexpr SettingsWindowSize settingsDefaultWindowSize() {
    return {860, 620};
}

constexpr SettingsWindowSize settingsMinimumWindowSize() {
    return {720, 520};
}

constexpr auto settingsActionLabels() {
    return std::array<std::string_view, 3>{"恢复修改", "应用",
                                          "保存并关闭"};
}

constexpr auto settingsClipboardActionLabels() {
    return std::array<std::string_view, 3>{"复制选中", "删除选中", "清空历史"};
}

constexpr auto settingsRuntimeStatusLabels() {
    return std::array<std::string_view, 4>{"fcitx5-remote", "Fcitx5 服务",
                                          "当前输入法", "ModernIME"};
}

constexpr auto settingsPageSurfaceStyleClasses() {
    return std::array<std::string_view, 4>{"modernime-page-scroller",
                                          "modernime-page-viewport",
                                          "modernime-page-stack",
                                          "modernime-page-surface"};
}

constexpr auto settingsKeyboardShortcuts() {
    return std::array<std::string_view, 3>{"<Primary>f",
                                          "<Primary>Return", "Escape"};
}

} // namespace modernime::settings
