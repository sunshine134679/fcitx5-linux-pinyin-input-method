#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    const auto *path = std::getenv("FAKE_FCITX5_LOG");
    if (path == nullptr || *path == '\0') {
        return EXIT_FAILURE;
    }

    std::ofstream log(path, std::ios::app);
    log << "args";
    for (int index = 1; index < argc; ++index) {
        log << ' ' << argv[index];
    }
    log << '\n';
    const auto *addonDirs = std::getenv("FCITX_ADDON_DIRS");
    log << "addon=" << (addonDirs == nullptr ? "" : addonDirs) << '\n';
    const auto *error = std::getenv("FAKE_FCITX5_ERROR");
    if (error != nullptr && *error != '\0') {
        std::cerr << error << '\n';
    }
    const auto *exitCode = std::getenv("FAKE_FCITX5_EXIT");
    return exitCode == nullptr ? EXIT_SUCCESS : std::atoi(exitCode);
}
