#include "modernime/settings/runtime_environment.h"
#include <fcitx-utils/standardpath.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

int main(int argc, char **argv) {
    const bool installed = argc == 2;
    const auto prefix = installed ? std::filesystem::path(argv[1]) :
        std::filesystem::temp_directory_path() /
        ("modernime-discovery-" + std::to_string(getpid()));
    const auto dataRoot = prefix / "share/fcitx5";
    const char *files[] = {"addon/modernime.conf", "addon/modernime-ui.conf",
                          "inputmethod/modernime.conf"};
    if (!installed) {
        for (const auto *file : files) {
            const auto target = dataRoot / file;
            std::filesystem::create_directories(target.parent_path());
            std::ofstream(target) << "[Addon]\nName=ModernIME\n";
        }
        unsetenv("XDG_DATA_DIRS");
        setenv("XDG_DATA_HOME", (prefix / "empty-user-data").c_str(), 1);
        const auto actualPrefix = modernime::settings::installationPrefix(
            prefix / "bin/modernime-settings", "/usr");
        if (actualPrefix != prefix) return EXIT_FAILURE;
        // Use the same environment builder as the settings reload action.
        for (const auto &[name, value] : modernime::settings::runtimeEnvironment(
                 actualPrefix, "/usr/lib/fcitx5")) {
            setenv(name.c_str(), value.c_str(), 1);
        }
    }
    const fcitx::StandardPath paths;
    bool found = true;
    for (const auto *file : files) {
        const auto actual = paths.locate(fcitx::StandardPath::Type::PkgData, file);
        if (actual != (dataRoot / file).string()) {
            std::cerr << "metadata discovery mismatch: " << file << " -> " << actual << '\n';
            found = false;
        }
    }
    if (!installed) std::filesystem::remove_all(prefix);
    return found ? EXIT_SUCCESS : EXIT_FAILURE;
}
