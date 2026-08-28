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
    std::string currentInputMethod;
    bool modernimeActive = false;
    std::string message;
};

struct RuntimeResult final {
    bool success = false;
    std::string message;
};

class RuntimeController final {
public:
    static RuntimeStatus probe(const std::filesystem::path &executable,
                               const Environment &environment = {});
    static RuntimeResult reload(const std::filesystem::path &fcitxExecutable,
                                const std::filesystem::path &remoteExecutable,
                                const Environment &environment = {});
};

} // namespace modernime::settings
