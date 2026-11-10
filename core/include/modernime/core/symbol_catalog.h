#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace modernime::core {

struct SymbolEntry {
    // The complete UTF-8 symbol text is the stable ID, including variation
    // selectors and paired punctuation. Persist IDs verbatim; use find() to
    // validate favorites. Entries and pointers remain valid for process life.
    std::string id;
    std::string text;
    std::string name;
    // punctuation, math, units, arrows, emoji, or other.
    std::string category;
    std::vector<std::string> aliases;
};

class SymbolCatalog final {
public:
    static const std::vector<SymbolEntry> &entries();
    static const SymbolEntry *find(std::string_view id);

    // Empty query/category means all. Favorites are returned first, preserving
    // catalog order within each group. Unknown IDs never produce new entries.
    // Search supports names, pinyin/English aliases and symbol substrings;
    // ASCII case and whitespace are ignored. No I/O or user state is owned here.
    static std::vector<const SymbolEntry *> search(
        std::string_view query, std::string_view category = {},
        const std::vector<std::string> &favoriteIds = {},
        bool favoritesOnly = false, std::size_t limit = 60);
};

// Exact legacy pinyin macro lookup. Values and their order are unchanged;
// date/time/week and numeric conversion remain the provider's responsibility.
std::vector<std::string> symbolMacroValues(std::string_view key);

} // namespace modernime::core
