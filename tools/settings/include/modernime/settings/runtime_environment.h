#pragma once

#include "modernime/settings/runtime_controller.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace modernime::settings {

inline std::filesystem::path installationPrefix(
    const std::filesystem::path &executable,
    const std::filesystem::path &fallback) {
    if (executable.filename() == "modernime-settings" &&
        executable.parent_path().filename() == "bin") {
        return executable.parent_path().parent_path();
    }
    return fallback;
}

inline std::filesystem::path currentInstallationPrefix(
    const Environment &environment, const std::filesystem::path &fallback) {
    for (const auto &[key, value] : environment) {
        if (key == "MODERNIME_PREFIX" && !value.empty()) return value;
    }
    if (const auto *prefix = std::getenv("MODERNIME_PREFIX"); prefix && *prefix) return prefix;
    std::error_code error;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", error).string();
    // 原子替换已运行的客户端后，Linux 会在旧可执行文件路径后标记 deleted。
    if (executable.ends_with(" (deleted)")) executable.resize(executable.size() - 10);
    return installationPrefix(executable, fallback);
}

inline std::string prependSearchPath(const std::filesystem::path &directory,
                                     const char *existing,
                                     std::string_view defaults = {}) {
    std::string result = directory.string();
    std::istringstream paths(existing != nullptr && *existing != '\0'
                                 ? std::string(existing) : std::string(defaults));
    std::string path;
    while (std::getline(paths, path, ':')) {
        if (!path.empty() && path != directory.string()) {
            result += ':' + path;
        }
    }
    return result;
}

inline Environment runtimeEnvironment(const std::filesystem::path &prefix,
                                      const std::string &systemAddonDirectory) {
    Environment environment;
    for (const char *name : {"DISPLAY", "WAYLAND_DISPLAY",
                             "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"}) {
        if (const auto *value = std::getenv(name); value != nullptr && *value != '\0') {
            environment.emplace_back(name, value);
        }
    }
    if (const auto *cfg = std::getenv("MODERNIME_CONFIG_HOME"); cfg != nullptr && *cfg != '\0') {
        environment.emplace_back("XDG_CONFIG_HOME", cfg);
    } else if (const auto *xdgCfg = std::getenv("XDG_CONFIG_HOME"); xdgCfg != nullptr && *xdgCfg != '\0') {
        environment.emplace_back("XDG_CONFIG_HOME", xdgCfg);
    }
    if (const auto *data = std::getenv("MODERNIME_DATA_HOME"); data != nullptr && *data != '\0') {
        environment.emplace_back("XDG_DATA_HOME", data);
    } else if (const auto *xdgData = std::getenv("XDG_DATA_HOME"); xdgData != nullptr && *xdgData != '\0') {
        environment.emplace_back("XDG_DATA_HOME", xdgData);
    }
    // Also cover lib64 and distribution multiarch directories after relocation.
    std::string addons = (prefix / "lib/fcitx5").string() + ':' +
                         (prefix / "lib64/fcitx5").string();
    std::error_code error;
    const auto libraryRoot = prefix / "lib";
    if (std::filesystem::is_directory(libraryRoot, error)) {
        for (std::filesystem::directory_iterator it(libraryRoot, error), end;
             !error && it != end; it.increment(error)) {
            if (std::filesystem::is_directory(it->path() / "fcitx5", error)) {
                addons += ':' + (it->path() / "fcitx5").string();
            }
        }
    }
    if (!systemAddonDirectory.empty()) addons += ':' + systemAddonDirectory;
    if (const auto *existing = std::getenv("FCITX_ADDON_DIRS"); existing && *existing) {
        addons += ':' + std::string(existing);
    }
    environment.emplace_back("FCITX_ADDON_DIRS", std::move(addons));
    environment.emplace_back("XDG_DATA_DIRS", prependSearchPath(
        prefix / "share", std::getenv("XDG_DATA_DIRS"), "/usr/local/share:/usr/share"));
    return environment;
}

} // namespace modernime::settings
