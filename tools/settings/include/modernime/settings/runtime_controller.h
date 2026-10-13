#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace modernime::settings {

using Environment = std::vector<std::pair<std::string, std::string>>;

struct RuntimeStatus final {
    bool available = false;
    bool running = false;
    bool inputContextAvailable = false;
    bool inputMethodEnabled = false;
    std::string currentInputMethod;
    bool modernimeAvailable = false;
    bool modernimeActive = false;
    std::string message;

    bool appliedHashAvailable = false;
    std::string appliedContentHash;
    std::uint64_t settingsGeneration = 0;
};

struct RuntimeResult final {
    bool success = false;
    std::string message;
};

class RuntimeController final {
public:
    static RuntimeStatus probe(const std::filesystem::path &executable,
                               const Environment &environment = {});
    static RuntimeStatus probe(const std::filesystem::path &executable,
                               const std::filesystem::path &runtimeStatePath,
                               const Environment &environment = {});
    static RuntimeResult reload(const std::filesystem::path &fcitxExecutable,
                                const std::filesystem::path &remoteExecutable,
                                const Environment &environment = {});
};

std::string describeSettingsApplication(const RuntimeStatus &status,
                                        std::string_view savedContentHash);

} // namespace modernime::settings
