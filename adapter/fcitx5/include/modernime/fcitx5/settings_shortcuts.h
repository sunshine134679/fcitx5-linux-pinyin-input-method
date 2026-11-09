#pragma once

#include <fcitx-utils/misc.h>
#include <array>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace modernime::fcitx5 {

struct SettingsShortcut {
    const char *page;
    const char *label;
    const char *icon;
};

inline constexpr std::array settingsShortcuts{
    SettingsShortcut{"overview", "ModernIME 设置", "preferences-system-symbolic"},
    SettingsShortcut{"dictionary", "个人词典", "accessories-dictionary-symbolic"},
    SettingsShortcut{"clipboard", "剪贴板历史", "edit-paste-symbolic"},
    SettingsShortcut{"diagnostics", "系统与诊断", "utilities-system-monitor-symbolic"},
};

// Resolve beside the loaded addon so a relocated installation opens its own
// client. lib/fcitx5, lib64/fcitx5 and lib/<triplet>/fcitx5 are supported.
inline std::string settingsExecutable(const void *moduleAnchor) {
    Dl_info info{};
    if (dladdr(moduleAnchor, &info) && info.dli_fname) {
        auto directory = std::filesystem::path(info.dli_fname).parent_path();
        if (directory.filename() == "fcitx5") {
            directory = directory.parent_path();
            for (int level = 0; level < 2; ++level) {
                if (directory.filename() == "lib" || directory.filename() == "lib64") {
                    const auto client = directory.parent_path() / "bin/modernime-settings";
                    if (::access(client.c_str(), X_OK) == 0) return client.string();
                    break;
                }
                directory = directory.parent_path();
            }
        }
    }
    return "modernime-settings";
}

inline void openSettingsPage(std::string_view page, const void *moduleAnchor) {
    fcitx::startProcess({settingsExecutable(moduleAnchor), "--page", std::string(page)});
}

} // namespace modernime::fcitx5
