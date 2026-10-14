#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modernime::settings {

struct PronunciationSuggestion final {
    std::vector<std::string> readings;
    bool approximate = false;
};

// Built lazily in a worker from the dictionaries already shipped with the IME.
// Compact text and row offsets avoid retaining a second dictionary engine or
// one heap-allocated hash entry for every word.
class DictionaryPronunciation final {
public:
    static DictionaryPronunciation load(
        const std::vector<std::filesystem::path> &paths,
        const std::function<bool()> &cancelled = {});
    PronunciationSuggestion suggest(std::string_view phrase) const;
    bool available() const { return !offsets_.empty(); }

private:
    std::string records_;
    std::vector<std::size_t> offsets_;
    std::unordered_map<std::uint32_t, std::string> characters_;
};

} // namespace modernime::settings
