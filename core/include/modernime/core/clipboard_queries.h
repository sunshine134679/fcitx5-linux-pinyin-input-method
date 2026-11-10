#pragma once

#include "modernime/core/clipboard_history.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

// Synchronous worker-side storage. Use a path distinct from the history file.
// Mutations read the latest file under ClipboardHistory's writer lock and only
// publish the new entries after a successful atomic 0600 save. Failed reads or
// writes keep the last visible entries; corrupt files are never auto-replaced.
// Calls and entries() access on one instance must be externally serialized.
class ClipboardFavorites final {
public:
    explicit ClipboardFavorites(std::filesystem::path path);

    bool load(std::string *error = nullptr);
    // Nonempty text, at most ClipboardHistory::kMaxEntryBytes. New entries are
    // newest first; duplicates succeed without reordering. A full file rejects
    // new entries instead of evicting a favorite.
    bool add(std::string_view text, std::string *error = nullptr);
    // Removes an exact full-text identity; a missing identity returns an error.
    bool remove(std::string_view expectedText, std::string *error = nullptr);
    bool clear(std::string *error = nullptr);

    const std::vector<std::string> &entries() const { return history_.entries(); }

private:
    bool update(const std::function<bool(ClipboardHistory &)> &mutation,
                std::string *error);

    std::filesystem::path path_;
    ClipboardHistory history_;
};

struct ClipboardSearchMatch {
    std::string_view text;
    bool favorite = false;
    std::optional<std::size_t> historyIndex;
};

// Pure substring search: ASCII case-insensitive, other UTF-8 bytes exact.
// Empty query matches all entries. Favorites precede history, retaining each
// input's order, and complete texts are deduplicated. historyIndex is the first
// exact matching position in the supplied history, even for a favorite result.
// Results borrow text from the input vectors; both vectors and their strings
// must stay alive and unchanged while results are used. Keep their immutable
// snapshots alive across asynchronous consumers. No clipboard text is copied.
std::vector<ClipboardSearchMatch> searchClipboard(
    const std::vector<std::string> &history,
    const std::vector<std::string> &favorites, std::string_view query,
    bool favoritesOnly = false, std::size_t limit = 60);

} // namespace modernime::core
