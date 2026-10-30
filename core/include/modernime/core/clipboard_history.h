#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
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

    // Atomically saves the history to a local file with owner-only access (0600).
    // Note: Production writers should use updateFile() to coordinate across processes.
    bool save(const std::filesystem::path &path,
              std::string *error = nullptr) const;

    // Atomically updates the history file under a stable cross-process advisory lock.
    // Loads the latest disk state, executes mutation, and writes the committed result
    // via a unique 0600 temporary file.
    static bool updateFile(
        const std::filesystem::path &path,
        const std::function<bool(ClipboardHistory &)> &mutation,
        ClipboardHistory *committedHistory = nullptr,
        std::string *error = nullptr,
        bool nonBlocking = false,
        int timeoutMs = 200);

    bool remove(std::size_t index);
    void clear();

    const std::vector<std::string> &entries() const { return entries_; }

private:
    std::vector<std::string> entries_;
};

} // namespace modernime::core
