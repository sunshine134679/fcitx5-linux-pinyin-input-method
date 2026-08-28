#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

class ClipboardHistory final {
public:
    static constexpr std::size_t kMaxEntries = 30;
    static constexpr std::size_t kMaxEntryBytes = 1024 * 1024;

    // Records a non-empty clipboard value at the front of the history.
    // Existing equal values are moved to the front instead of duplicated.
    // Returns whether the visible history order changed.
    bool observe(std::string_view text);

    // Loads the history from a local file. A missing file is treated as empty.
    bool load(const std::filesystem::path &path, std::string *error = nullptr);

    // Atomically saves the history to a local file with owner-only access.
    bool save(const std::filesystem::path &path,
              std::string *error = nullptr) const;

    bool remove(std::size_t index);
    void clear();

    const std::vector<std::string> &entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

} // namespace modernime::core
