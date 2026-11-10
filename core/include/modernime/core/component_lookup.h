#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::core {

struct ComponentCandidate {
    std::string text;
    std::string components;
};

// Immutable after construction. Construct outside the per-keystroke path;
// lookup only reads the prebuilt index and never touches disk.
class ComponentLookup {
public:
    explicit ComponentLookup(std::filesystem::path path = defaultPath());

    static std::filesystem::path defaultPath();
    std::vector<ComponentCandidate> lookup(std::string_view query,
                                           std::size_t limit = 30) const;
    bool available() const noexcept;
    const std::string &loadError() const noexcept;

private:
    std::unordered_map<std::string, std::vector<ComponentCandidate>> index_;
    std::string loadError_;
};

} // namespace modernime::core
